#ifndef _TYPES_H_
#define _TYPES_H_

#define MAX_NAME_LEN 128    
#define MAX_DIRECT_ACC 6
#define MAX_FILE_NUM 512
#define INODE_MAP_SZ 64     // MAX_FILE_NUM / 8
#define DATA_MAP_SZ 384     // MAX_FILE_NUM * MAX_DIRECT_ACC / 8

#define ROUND_DOWN(value, round) (value % round == 0 ? value : (value / round) * round)
#define ROUND_UP(value, round) (value % round == 0 ? value : (value / round + 1) * round)
#define CEIL(value, round) (value % round == 0 ? (value / round) : (value / round + 1))

struct custom_options {
	char*        device;
};

typedef enum file_type {
    MYFILE,     // 普通文件
    DIR         // 目录文件
} FILE_TYPE;

struct newfs_super {
    // 驱动信息
    int fd;
    int dev_io_sz;
    int dev_disk_sz;

    // 文件系统信息
    uint32_t magic;         // 文件系统标识
    int max_ino;            // 最多支持的文件数
    // inode位图信息
    uint8_t* map_inode;     // inode位图内存地址
    int map_inode_blks;     // inode位图占用的磁盘块数
    int map_inode_offset;   // inode位图在磁盘上的偏移
    // data位图信息
    uint8_t* map_data;      // data位图内存地址
    int map_data_blks;      // data位图占用的磁盘块数
    int map_data_offset;    // data位图在磁盘上的偏移

    int inode_offset;       // inode起始磁盘偏移
    int data_offset;        // data起始磁盘偏移

    // 根目录
    struct newfs_dentry* root_dentry;   // 根目录内存地址
};

struct newfs_inode {
    int ino;                // 在inode位图中的下标
    int size;               // 文件已占用空间

    FILE_TYPE ftype;        // 文件类型（目录类型、普通文件类型）
    int dir_cnt;            // 如果是目录类型文件，下面有几个目录项

    struct newfs_dentry* dentry;        // 指向该inode的dentry内存地址
    struct newfs_dentry* dentrys;       // 所有目录项内存起始地址

    int block_pointer[MAX_DIRECT_ACC];  // 数据块号          
};

struct newfs_dentry {
    char name[MAX_NAME_LEN];        // 指向的ino文件名
    int ino;                        // 指向的ino号
    FILE_TYPE ftype;                // 指向的ino文件类型
    struct newfs_inode* inode;      // 指向的inode内存地址
};

#endif /* _TYPES_H_ */