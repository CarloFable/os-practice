#include "newfs.h"

/******************************************************************************
* SECTION: 宏定义
*******************************************************************************/
#define OPTION(t, p)        { t, offsetof(struct custom_options, p), 1 }

/******************************************************************************
* SECTION: 全局变量
*******************************************************************************/
static const struct fuse_opt option_spec[] = {		/* 用于FUSE文件系统解析参数 */
	OPTION("--device=%s", device),
	FUSE_OPT_END
};

struct custom_options newfs_options;			 /* 全局选项 */
struct newfs_super super;

/******************************************************************************
* SECTION: FUSE操作定义
*******************************************************************************/
static struct fuse_operations operations = {
	.init = newfs_init,						 /* mount文件系统 */		
	.destroy = newfs_destroy,				 /* umount文件系统 */
	.mkdir = newfs_mkdir,					 /* 建目录，mkdir */
	.getattr = newfs_getattr,				 /* 获取文件属性，类似stat，必须完成 */
	.readdir = newfs_readdir,				 /* 填充dentrys */
	.mknod = newfs_mknod,					 /* 创建文件，touch相关 */
	.write = NULL,								  	 /* 写入文件 */
	.read = NULL,								  	 /* 读文件 */
	.utimens = newfs_utimens,				 /* 修改时间，忽略，避免touch报错 */
	.truncate = NULL,						  		 /* 改变文件大小 */
	.unlink = NULL,							  		 /* 删除文件 */
	.rmdir	= NULL,							  		 /* 删除目录， rm -r */
	.rename = NULL,							  		 /* 重命名，mv */

	.open = NULL,							
	.opendir = NULL,
	.access = NULL
};

// 读出驱动磁盘块
void newfs_driver_read(int offset, int size, uint8_t* out)
{
    int offset_aligned = ROUND_DOWN(offset, super.dev_io_sz);  		// 前对齐
    int bias = offset - offset_aligned;
    int size_aligned = ROUND_UP((size + bias), super.dev_io_sz);  	// 后对齐
    uint8_t* buf = (uint8_t*)malloc(size_aligned);

    ddriver_seek(super.fd, offset_aligned, SEEK_SET);
    for(uint8_t* cur = buf; size_aligned > 0; cur += super.dev_io_sz, size_aligned -= super.dev_io_sz)
        ddriver_read(super.fd, (char*)cur, super.dev_io_sz);

    memcpy(out, buf + bias, size);
    free(buf);
}

// 读出后写回驱动磁盘块
void newfs_driver_write(int offset, int size, uint8_t* in)
{
    int offset_aligned = ROUND_DOWN(offset, super.dev_io_sz);  		// 前对齐
    int bias = offset - offset_aligned;
    int size_aligned = ROUND_UP((size + bias), super.dev_io_sz);  	// 后对齐
    uint8_t* buf = (uint8_t*)malloc(size_aligned);

    newfs_driver_read(offset_aligned, size_aligned, buf);
    memcpy(buf + bias, in, size);
    
    ddriver_seek(super.fd, offset_aligned, SEEK_SET);
    for(uint8_t* cur = buf; size_aligned > 0; cur += super.dev_io_sz, size_aligned -= super.dev_io_sz)
        ddriver_write(super.fd, (char*)cur, super.dev_io_sz);

    free(buf);
}

// 新建目录项
struct newfs_dentry* new_dentry(char* fname, FILE_TYPE type)
{
	struct newfs_dentry* dentry = (struct newfs_dentry*)malloc(sizeof(struct newfs_dentry));
    memset(dentry, 0, sizeof(struct newfs_dentry));
    strcpy(dentry->name, fname);
	dentry->ino = -1;
	dentry->ftype = type;
	return dentry;
}

// 新建索引结点
struct newfs_inode* new_inode(FILE_TYPE type)
{
	struct newfs_inode* inode = (struct newfs_inode*)malloc(sizeof(struct newfs_inode));
    memset(inode, 0, sizeof(struct newfs_inode));
	inode->ftype = type;

	// 查inode位图
	for(int i = 0; i < INODE_MAP_SZ; ++i)
	{
		for(int j = 0; j < 8; ++j)
		{
			if((super.map_inode[i] >> j) == 0)
			{
				super.map_inode[i] |= (1 << j); 
				inode->ino = i * 8 + j;
				return inode;
			}
		}
	}
	
	return inode;
}

// 构建目录树
void dentry_tree(struct newfs_dentry* cur)
{
	// 查inode位图
	if(super.map_inode[cur->ino / 8] & (1 << (cur->ino % 8)))
	{
		struct newfs_inode* inode = (struct newfs_inode*)malloc(sizeof(struct newfs_inode));
		newfs_driver_read(super.inode_offset + cur->ino * sizeof(struct newfs_inode), sizeof(struct newfs_inode), (uint8_t*)inode);

		cur->inode = inode;
		inode->dentry = cur;

		int dentry_num = inode->dir_cnt;
		// 是目录，读子目录项
		if(inode->ftype == DIR && inode->size > 0)
		{	
			struct newfs_dentry* dentrys = (struct newfs_dentry*)malloc(dentry_num * sizeof(struct newfs_dentry));
			inode->dentrys = dentrys;
			int cnt = dentry_num;
			int blk_num = CEIL(inode->size, (2 * super.dev_io_sz));
			for(int i = 0; i < blk_num; ++i)
			{
				uint8_t* data_blk = (uint8_t*)malloc(2 * super.dev_io_sz);
				newfs_driver_read(super.data_offset + inode->block_pointer[i] * 2 * super.dev_io_sz, 2 * super.dev_io_sz, data_blk);

				for(int j = 0; j < 2 * super.dev_io_sz && cnt > 0; j += sizeof(struct newfs_dentry))
				{
					int ofs = j / sizeof(struct newfs_dentry);
					memcpy(dentrys + ofs , (struct newfs_dentry*)data_blk + ofs, sizeof(struct newfs_dentry));
					--cnt;
				}
				dentrys += 2 * super.dev_io_sz / sizeof(struct newfs_dentry);

				free(data_blk);
			}
		}
		// 递归进入子目录项
		for(int i = 0; i < dentry_num; ++i)
			dentry_tree(inode->dentrys + i);
	}
}

// 释放目录树
void free_tree(struct newfs_dentry* cur)
{
	struct newfs_inode* inode;
	if(cur != NULL)
	{
		inode = cur->inode;
		free(cur);
	}

	if(inode->ftype == DIR && inode->size > 0)
	{
		int dentry_num = inode->dir_cnt;
		for(int i = 0; i < dentry_num; ++i)
			free_tree(inode->dentrys + i);
	}
	free(inode);	
}

// 计算目录层级
int dir_level(char* path)
{
	char* c = path;
    int level = 0;
	
	// 根目录
    if (strcmp(path, "/") == 0)
        return level;

    while (*c != '\0')
	{
        if (*c == '/')
            ++level;
        ++c;
    }
    return level;
}

// 解析路径
struct newfs_dentry* parse(char* path, int* find_flag, int* root_flag)
{
	struct newfs_dentry* cur = super.root_dentry;
	*find_flag = 0;
    *root_flag = 0;

	// 根目录
	int total_level = dir_level(path);
    if (total_level == 0)
	{
        *find_flag = 1;
        *root_flag = 1;
        return cur;
    }

	// 以/为分隔符每次调用依次返回子串，遍历每个子串
	int level = 0;
	char* path1 = (char*)malloc(sizeof(path));
	strcpy(path1, path); 
    char* fname = strtok(path1, "/");    
    while (fname)
    {   
        ++level;
		struct newfs_dentry* obj = cur->inode->dentrys;
		int hit = 0;
		int dentry_num = cur->inode->dir_cnt;
		for(int i = 0; i < dentry_num; ++i, ++obj)
		{
			if(!strcmp(obj->name, fname))
			{
				hit = 1;
				// 文件
				if (obj->ftype == MYFILE)
				{
					// 路径中间是文件，提示路径有误并返回该文件目录项
					if(level < total_level)
					{
						//printf("fname: %s  is not a dir in middle of path: %s\n", fname, path);
						return obj;
					}
					else if(level == total_level)
					{
						*find_flag = 1;
						return obj;
					}
				}
				// 目录
				else if (obj->ftype == DIR)
				{
					if(level < total_level)
						break;
					else if(level == total_level)
					{
						*find_flag = 1;
						return obj;
					}
				}
			}
		}
		// 当前层级未命中目录项，提示路径有误并返回当前已命中目录项
		if(!hit)
		{
			//printf("fname: %s is not found in path: %s\n", fname, path);
			return cur;
		}
		cur = obj;
        fname = strtok(NULL, "/"); 
    }
	return cur;
}

// 获得路径中的（最后一级）文件名
char* get_fname(char* path)
{
    return strrchr(path, '/') + 1;
}

// 新建

/******************************************************************************
* SECTION: 必做函数实现
*******************************************************************************/
/**
 * @brief 挂载（mount）文件系统
 * 
 * @param conn_info 可忽略，一些建立连接相关的信息 
 * @return void*
 */
void* newfs_init(struct fuse_conn_info* conn_info)
{
	// 超级块-驱动信息
	super.fd = ddriver_open(newfs_options.device);
	ddriver_ioctl(super.fd, IOC_REQ_DEVICE_SIZE,  &super.dev_disk_sz);
    ddriver_ioctl(super.fd, IOC_REQ_DEVICE_IO_SZ, &super.dev_io_sz);
	// 读超级块
	int fd_tmp = super.fd;
	int disk_sz = super.dev_disk_sz;
	int io_sz = super.dev_io_sz; 
	newfs_driver_read(0, sizeof(struct newfs_super), (uint8_t*)(&super));
	super.fd = fd_tmp;
	super.dev_disk_sz = disk_sz;
	super.dev_io_sz = io_sz;

	// 非本文件系统标识，初始化文件系统
	int init_flag = 0;
	if(super.magic != NEWFS_MAGIC)
	{	
		super.max_ino = MAX_FILE_NUM;
		// 索引位图
		int super_blks = CEIL(sizeof(struct newfs_super), io_sz);
		super.map_inode_offset = super_blks * io_sz;
		int inode_map_blks = CEIL(INODE_MAP_SZ, io_sz);
		super.map_inode_blks = inode_map_blks;
		// 数据位图
		super.map_data_offset = (super_blks + inode_map_blks) * io_sz;
		int data_map_blks = CEIL(DATA_MAP_SZ, io_sz);
        super.map_data_blks = data_map_blks;
		// inode
		super.inode_offset = (super_blks + inode_map_blks + data_map_blks) * io_sz;
		int inode_blks = CEIL(MAX_FILE_NUM * sizeof(struct newfs_inode), io_sz);
		// data
		super.data_offset = (super_blks + inode_map_blks + data_map_blks + inode_blks) * io_sz;
		init_flag = 1;
	}

	// inode位图读入内存
	super.map_inode = (uint8_t*)malloc(INODE_MAP_SZ);
	// 非本文件系统标识，初始化inode位图
	if(init_flag)
		memset(super.map_inode, 0, INODE_MAP_SZ);
	else
		newfs_driver_read(super.map_inode_offset, INODE_MAP_SZ, super.map_inode);
	// data位图读入内存
	super.map_data = (uint8_t*)malloc(DATA_MAP_SZ);
	// 非本文件系统标识，初始化data位图
	if(init_flag)
		memset(super.map_data, 0, DATA_MAP_SZ);
	else
		newfs_driver_read(super.map_data_offset, DATA_MAP_SZ, super.map_data);

	// 根目录读入内存
	// 非本文件系统标识，初始化根目录
	if(init_flag)
	{
		struct newfs_dentry* root_dir = new_dentry("/", DIR);	// 目录项：根目录
		struct newfs_inode* root_inode = new_inode(DIR);		// 索引结点：根目录

		root_dir->ino = root_inode->ino;
		root_inode->dentry = root_dir;

		newfs_driver_write(super.inode_offset, sizeof(struct newfs_inode), (uint8_t*)root_inode);
		free(root_inode);

		super.map_data[0] |= 1;
		newfs_driver_write(super.data_offset, sizeof(struct newfs_dentry), (uint8_t*)root_dir);
		free(root_dir);
	}
	struct newfs_dentry* root_dir = (struct newfs_dentry*)malloc(sizeof(struct newfs_dentry));
	newfs_driver_read(super.data_offset, sizeof(struct newfs_dentry), (uint8_t*)root_dir);
	super.root_dentry = root_dir;

	// 从根目录开始，将目录树读入内存
	dentry_tree(root_dir);
	
	return NULL;
}

/**
 * @brief 卸载（umount）文件系统
 * 
 * @param p 可忽略
 * @return void
 */
void newfs_destroy(void* p)
{
	// 将内存中的超级块和位图写回磁盘
	super.magic = NEWFS_MAGIC;
	newfs_driver_write(0, sizeof(struct newfs_super), (uint8_t*)(&super));

	newfs_driver_write(super.map_inode_offset, INODE_MAP_SZ, super.map_inode);
	free(super.map_inode);

	newfs_driver_write(super.map_data_offset, DATA_MAP_SZ, super.map_data);
	free(super.map_data);

	free_tree(super.root_dentry);

	ddriver_close(super.fd);
}

/**
 * @brief 创建目录
 * 
 * @param path 相对于挂载点的路径
 * @param mode 创建模式（只读？只写？），可忽略
 * @return int 0成功，否则失败
 */
int newfs_mkdir(const char* path, mode_t mode)
{
	(void)mode;
	int	find_flag, root_flag;
	struct newfs_dentry* last_dentry = parse(path, &find_flag, &root_flag);
	struct newfs_inode* last_inode = last_dentry->inode;

	// 目标路径已存在（新建路径应该不存在，parse会截断到命中的上级目录）
	if (find_flag)
		return -EEXIST;
	// 目标路径上级目录是文件，不能创建目录
	if (last_dentry->ftype == MYFILE)
		return -ENXIO;

	// 创建目录项-创建索引结点-将目录项写入上级目录数据块
	char* fname = get_fname(path);

	struct newfs_dentry* dentry = new_dentry(fname, DIR);
	struct newfs_inode* inode = new_inode(DIR);
	
	dentry->ino = inode->ino;
	dentry->inode = inode;

	inode->dentry = dentry;
	newfs_driver_write(super.inode_offset + inode->ino * sizeof(struct newfs_inode), sizeof(struct newfs_inode), (uint8_t*)inode);

	// 更新上级目录信息
	// 若写入新目录项后溢出数据块，则新取一个数据块
	int blks = CEIL(last_inode->size, (2 * super.dev_io_sz));
	int left = last_inode->size % (2 * super.dev_io_sz);
	if((left == 0) || ((left + sizeof(struct newfs_dentry)) > (2 * super.dev_io_sz)))
	{
		if(blks >= MAX_DIRECT_ACC)
			return -ENOSPC;

		// 查data位图
		int flag = 0;
		for(int i = 0; i < DATA_MAP_SZ; ++i)
		{
			for(int j = 0; j < 8; ++j)
			{
				if((super.map_data[i] >> j) == 0)
				{
					flag = 1;
					super.map_data[i] |= (1 << j); 
					last_inode->block_pointer[blks] = i * 8 + j;
					break;
				}
			}
			if(flag)
				break;
		}
		// 将新目录项写入新取data块
		int offset = super.data_offset + 2 * super.dev_io_sz * last_inode->block_pointer[blks];
		newfs_driver_write(offset, sizeof(struct newfs_dentry), (uint8_t*)dentry);
		// 更新上级目录信息
		last_inode->size = blks * 2 * super.dev_io_sz + sizeof(struct newfs_dentry);
	}
	// 未溢出
	else
	{
		// 将新目录项写入末data块
		int offset = super.data_offset + 2 * super.dev_io_sz * last_inode->block_pointer[blks - 1] + left;
		newfs_driver_write(offset, sizeof(struct newfs_dentry), (uint8_t*)dentry);
		// 更新上级目录信息
		last_inode->size += sizeof(struct newfs_dentry);
	}
	// 更新上级目录信息
	++(last_inode->dir_cnt);
	int dentry_num = last_inode->dir_cnt;
	struct newfs_dentry* dentrys = (struct newfs_dentry*)malloc(dentry_num * sizeof(struct newfs_dentry));
	--dentry_num;
	memcpy(dentrys, last_inode->dentrys, dentry_num * sizeof(struct newfs_dentry));
	memcpy(dentrys + dentry_num, dentry, sizeof(struct newfs_dentry));
	free(last_inode->dentrys);
	last_inode->dentrys = dentrys;
	// 将上级目录inode写回磁盘
	int offset = super.inode_offset + last_inode->ino * sizeof(struct newfs_inode);
	newfs_driver_write(offset, sizeof(struct newfs_inode), (uint8_t*)last_inode);
	
	return 0;
}

/**
 * @brief 获取文件或目录的属性，该函数非常重要
 * 
 * @param path 相对于挂载点的路径
 * @param newfs_stat 返回状态
 * @return int 0成功，否则失败
 */
int newfs_getattr(const char* path, struct stat* newfs_stat)
{
	int	find_flag, root_flag;
	struct newfs_dentry* dentry = parse(path, &find_flag, &root_flag);
	
	// 路径不存在
	if (!find_flag)
		return -ENOENT;

	// 目录
	if (dentry->ftype == DIR)
	{
		newfs_stat->st_mode = S_IFDIR;
		newfs_stat->st_size = dentry->inode->size;
	}
	// 文件
	else if (dentry->ftype == MYFILE)
	{
		newfs_stat->st_mode = S_IFREG;
		newfs_stat->st_size = dentry->inode->size;
	}	

	newfs_stat->st_nlink = 1;
	newfs_stat->st_uid = getuid();
	newfs_stat->st_gid = getgid();
	newfs_stat->st_atime = time(NULL);
	newfs_stat->st_mtime = time(NULL);
	newfs_stat->st_blksize = super.dev_io_sz;

	// 根目录
	if (root_flag)
	{
		newfs_stat->st_size	= super.root_dentry->inode->size; 
		newfs_stat->st_blocks = super.dev_disk_sz / super.dev_io_sz;
		newfs_stat->st_nlink  = 2;
	}

	return 0;
}

/**
 * @brief 遍历目录项，填充至buf，并交给FUSE输出
 * 
 * @param path 相对于挂载点的路径
 * @param buf 输出buffer
 * @param filler 参数讲解:
 * 
 * typedef int (*fuse_fill_dir_t) (void *buf, const char *name,
 *				const struct stat *stbuf, off_t off)
 * buf: name会被复制到buf中
 * name: dentry名字
 * stbuf: 文件状态，可忽略
 * off: 下一次offset从哪里开始，这里可以理解为第几个dentry
 * 
 * @param offset 第几个目录项？
 * @param fi 可忽略
 * @return int 0成功，否则失败
 */
int newfs_readdir(const char * path, void * buf, fuse_fill_dir_t filler, off_t offset,
			    		 struct fuse_file_info * fi) {
    int	find_flag, root_flag;
	struct newfs_dentry* dentry = parse(path, &find_flag, &root_flag);
	struct newfs_dentry* sub_dentry;
	struct newfs_inode* inode;

	if (find_flag)
	{
		inode = dentry->inode;
		sub_dentry = inode->dentrys + offset;
		if (offset < inode->dir_cnt)
			filler(buf, sub_dentry->name, NULL, ++offset);
		return 0;
	}
	return -ENOENT;
}

/**
 * @brief 创建文件
 * 
 * @param path 相对于挂载点的路径
 * @param mode 创建文件的模式，可忽略
 * @param dev 设备类型，可忽略
 * @return int 0成功，否则失败
 */
int newfs_mknod(const char* path, mode_t mode, dev_t dev)
{
	int	find_flag, root_flag;
	struct newfs_dentry* last_dentry = parse(path, &find_flag, &root_flag);
	struct newfs_inode* last_inode = last_dentry->inode;

	// 目标路径已存在（新建路径应该不存在，parse会截断到命中的上级目录）
	if (find_flag)
		return -EEXIST;
	// 目标路径上级目录是文件，不能创建文件
	if (last_dentry->ftype == MYFILE)
		return -ENXIO;

	// 创建目录项-创建索引结点-将目录项写入上级目录数据块
	char* fname = get_fname(path);

	struct newfs_dentry* dentry = new_dentry(fname, MYFILE);
	struct newfs_inode* inode = new_inode(MYFILE);
	
	dentry->ino = inode->ino;
	dentry->inode = inode;

	inode->dentry = dentry;
	newfs_driver_write(super.inode_offset + inode->ino * sizeof(struct newfs_inode), sizeof(struct newfs_inode), (uint8_t*)inode);

	// 更新上级目录信息
	// 若写入新目录项后溢出数据块，则新取一个数据块
	int blks = CEIL(last_inode->size, (2 * super.dev_io_sz));
	int left = last_inode->size % (2 * super.dev_io_sz);
	if((left == 0) || ((left + sizeof(struct newfs_dentry)) > (2 * super.dev_io_sz)))
	{
		if(blks >= MAX_DIRECT_ACC)
			return -ENOSPC;

		// 查data位图
		int flag = 0;
		for(int i = 0; i < DATA_MAP_SZ; ++i)
		{
			for(int j = 0; j < 8; ++j)
			{
				if((super.map_data[i] >> j) == 0)
				{
					flag = 1;
					super.map_data[i] |= (1 << j); 
					last_inode->block_pointer[blks] = i * 8 + j;
					break;
				}
			}
			if(flag)
				break;
		}
		// 将新目录项写入新取data块
		int offset = super.data_offset + 2 * super.dev_io_sz * last_inode->block_pointer[blks];
		newfs_driver_write(offset, sizeof(struct newfs_dentry), (uint8_t*)dentry);
		// 更新上级目录信息
		last_inode->size = blks * 2 * super.dev_io_sz + sizeof(struct newfs_dentry);
	}
	// 未溢出
	else
	{
		// 将新目录项写入末data块
		int offset = super.data_offset + 2 * super.dev_io_sz * last_inode->block_pointer[blks - 1] + left;
		newfs_driver_write(offset, sizeof(struct newfs_dentry), (uint8_t*)dentry);
		// 更新上级目录信息
		last_inode->size += sizeof(struct newfs_dentry);
	}
	// 更新上级目录信息
	++(last_inode->dir_cnt);
	int dentry_num = last_inode->dir_cnt;
	struct newfs_dentry* dentrys = (struct newfs_dentry*)malloc(dentry_num * sizeof(struct newfs_dentry));
	--dentry_num;
	memcpy(dentrys, last_inode->dentrys, dentry_num * sizeof(struct newfs_dentry));
	memcpy(dentrys + dentry_num, dentry, sizeof(struct newfs_dentry));
	free(last_inode->dentrys);
	last_inode->dentrys = dentrys;
	// 将上级目录inode写回磁盘
	int offset = super.inode_offset + last_inode->ino * sizeof(struct newfs_inode);
	newfs_driver_write(offset, sizeof(struct newfs_inode), (uint8_t*)last_inode);
	
	return 0;
}

/**
 * @brief 修改时间，为了不让touch报错 
 * 
 * @param path 相对于挂载点的路径
 * @param tv 实践
 * @return int 0成功，否则失败
 */
int newfs_utimens(const char* path, const struct timespec tv[2]) {
	(void)path;
	return 0;
}
/******************************************************************************
* SECTION: 选做函数实现
*******************************************************************************/
/**
 * @brief 写入文件
 * 
 * @param path 相对于挂载点的路径
 * @param buf 写入的内容
 * @param size 写入的字节数
 * @param offset 相对文件的偏移
 * @param fi 可忽略
 * @return int 写入大小
 */
int newfs_write(const char* path, const char* buf, size_t size, off_t offset,
		        struct fuse_file_info* fi) {
	/* 选做 */
	return size;
}

/**
 * @brief 读取文件
 * 
 * @param path 相对于挂载点的路径
 * @param buf 读取的内容
 * @param size 读取的字节数
 * @param offset 相对文件的偏移
 * @param fi 可忽略
 * @return int 读取大小
 */
int newfs_read(const char* path, char* buf, size_t size, off_t offset,
		       struct fuse_file_info* fi) {
	/* 选做 */
	return size;			   
}

/**
 * @brief 删除文件
 * 
 * @param path 相对于挂载点的路径
 * @return int 0成功，否则失败
 */
int newfs_unlink(const char* path) {
	/* 选做 */
	return 0;
}

/**
 * @brief 删除目录
 * 
 * 一个可能的删除目录操作如下：
 * rm ./tests/mnt/j/ -r
 *  1) Step 1. rm ./tests/mnt/j/j
 *  2) Step 2. rm ./tests/mnt/j
 * 即，先删除最深层的文件，再删除目录文件本身
 * 
 * @param path 相对于挂载点的路径
 * @return int 0成功，否则失败
 */
int newfs_rmdir(const char* path) {
	/* 选做 */
	return 0;
}

/**
 * @brief 重命名文件 
 * 
 * @param from 源文件路径
 * @param to 目标文件路径
 * @return int 0成功，否则失败
 */
int newfs_rename(const char* from, const char* to) {
	/* 选做 */
	return 0;
}

/**
 * @brief 打开文件，可以在这里维护fi的信息，例如，fi->fh可以理解为一个64位指针，可以把自己想保存的数据结构
 * 保存在fh中
 * 
 * @param path 相对于挂载点的路径
 * @param fi 文件信息
 * @return int 0成功，否则失败
 */
int newfs_open(const char* path, struct fuse_file_info* fi) {
	/* 选做 */
	return 0;
}

/**
 * @brief 打开目录文件
 * 
 * @param path 相对于挂载点的路径
 * @param fi 文件信息
 * @return int 0成功，否则失败
 */
int newfs_opendir(const char* path, struct fuse_file_info* fi) {
	/* 选做 */
	return 0;
}

/**
 * @brief 改变文件大小
 * 
 * @param path 相对于挂载点的路径
 * @param offset 改变后文件大小
 * @return int 0成功，否则失败
 */
int newfs_truncate(const char* path, off_t offset) {
	/* 选做 */
	return 0;
}


/**
 * @brief 访问文件，因为读写文件时需要查看权限
 * 
 * @param path 相对于挂载点的路径
 * @param type 访问类别
 * R_OK: Test for read permission. 
 * W_OK: Test for write permission.
 * X_OK: Test for execute permission.
 * F_OK: Test for existence. 
 * 
 * @return int 0成功，否则失败
 */
int newfs_access(const char* path, int type) {
	/* 选做: 解析路径，判断是否存在 */
	return 0;
}	
/******************************************************************************
* SECTION: FUSE入口
*******************************************************************************/
int main(int argc, char **argv)
{
    int ret;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	newfs_options.device = strdup("/home/guests/190110918/ddriver");

	if (fuse_opt_parse(&args, &newfs_options, option_spec, NULL) == -1)
		return -1;
	
	ret = fuse_main(args.argc, args.argv, &operations, NULL);
	fuse_opt_free_args(&args);
	return ret;
}