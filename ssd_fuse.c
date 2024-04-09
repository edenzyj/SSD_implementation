/*
  FUSE ssd: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "ssd_fuse_header.h"
#define SSD_NAME       "ssd_file"
enum
{
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};


static size_t physic_size;
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;

unsigned int page_number;

typedef union pca_rule PCA_RULE;
union pca_rule
{
    unsigned int pca;
    struct
    {
        unsigned int page : 16;
        unsigned int block: 16;
    } fields;
};

PCA_RULE curr_pca;

unsigned int* L2P;

unsigned int* P2L;

struct queue
{
    unsigned int* arr;
    unsigned int size;
};

struct queue PCA_Empty;

static void enqueue(struct queue q, unsigned int val)
{
    q.arr[q.size] = val;
    return;
}

static void dequeue(struct queue q)
{
    for(int i = 1; i < q.size; i++){
        q.arr[i-1] = q.arr[i];
    }
    return;
}

struct priority_array
{
    bool** arr;
    unsigned int* cnt;
    unsigned int min_one;
    unsigned int min_two;
};

struct priority_array PCA_Used;

static void comparison(struct priority_array pa)
{
    int small_one = page_number + 1;
    int small_two = page_number + 1;

    for(int i = 0; i < PHYSICAL_NAND_NUM; i++)
    {
        if(i == curr_pca.fields.block) continue;
        if(pa.cnt[i] < small_one)
        {
            pa.min_two = pa.min_one;
            pa.min_one = i;
            small_two = small_one;
            small_one = pa.cnt[i];
        }
        else if(pa.cnt[i] < small_two)
        {
            pa.min_two = i;
            small_two = pa.cnt[i];
        }
    }
    return;
}

static int ssd_resize(size_t new_size)
{
    //set logic size to new_size
    if (new_size >= LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 )
    {
        return -ENOMEM;
    }
    else
    {
        logic_size = new_size;
        return 0;
    }

}

static int ssd_expand(size_t new_size)
{
    //logical size must be less than logic limit

    if (new_size > logic_size)
    {
        return ssd_resize(new_size);
    }

    return 0;
}

static int nand_read(char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    //read from nand
    if ( (fptr = fopen(nand_name, "r") ))
    {
        fseek(fptr, my_pca.fields.page * 512, SEEK_SET);
        fread(buf, 1, 512, fptr);
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand read pca = %d\n", pca);
        return -EINVAL;
    }
    return 512;
}
static int nand_write(const char* buf, int pca, int size)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    //write to nand
    if ( (fptr = fopen(nand_name, "r+")))
    {
        fseek( fptr, my_pca.fields.page * 512, SEEK_SET );
        fwrite(buf, 1, size, fptr);
        fclose(fptr);
        physic_size ++;
    }
    else
    {
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        return -EINVAL;
    }

    nand_write_size += 512;
    return 512;
}

static int nand_erase(int block)
{
    char nand_name[100];
	int found = 0;
    FILE* fptr;

    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, block);

    //erase nand
    if ( (fptr = fopen(nand_name, "w")))
    {
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand (%s) erase nand = %d, return %d\n", nand_name, block, -EINVAL);
        return -EINVAL;
    }


	if (found == 0)
	{
		printf("nand erase not found\n");
		return -EINVAL;
	}

    printf("nand erase %d pass\n", block);
    return 1;
}

static void gc_move(unsigned int nand, unsigned int page)
{
    PCA_RULE pca;
    
    pca.fields.block = nand;
    pca.fields.page = page;
    char* temp_buf = malloc(512 * sizeof(char));

    // read and maintain related infornmation
    int mv_size = nand_read(temp_buf, pca.pca);
    L2P[P2L[pca.fields.block * 20 + pca.fields.page]] = curr_pca.pca;
    P2L[curr_pca.fields.block * 20 + curr_pca.fields.page] = P2L[pca.fields.block * 20 + pca.fields.page];
    P2L[pca.fields.block * 20 + pca.fields.page] = INVALID_PCA;
    PCA_Used.arr[nand][page] = false;

    // write and maintain related infornmation
    mv_size = nand_write(temp_buf, curr_pca.pca, mv_size);
    PCA_Used.arr[curr_pca.fields.block][curr_pca.fields.page] = true;
    PCA_Used.cnt[curr_pca.fields.block] += 1;
    curr_pca.fields.page += 1;
}

static void ftl_gc()
{
    // get the two nand number with minimal used size
    comparison(PCA_Used);

    // no need to do garbage collection
    if(PCA_Used.cnt[PCA_Used.min_one] == page_number) return;

    // clean two nands if it can.
    if(PCA_Used.cnt[PCA_Used.min_one] + PCA_Used.cnt[PCA_Used.min_two] <= page_number)
    {
        for(int i = 0; i < page_number; i++)
        {
            if(PCA_Used.arr[PCA_Used.min_two][i])
            {
                gc_move(PCA_Used.min_two, i);
            }
        }
        nand_erase(PCA_Used.min_two);
        PCA_Used.cnt[PCA_Used.min_two] = 0;
        enqueue(PCA_Empty, PCA_Used.min_two);
        PCA_Empty.size += 1;
        printf("Clean two nands~\n");
    }
    
    for(int i = 0; i < page_number; i++)
    {
        if(PCA_Used.arr[PCA_Used.min_one][i])
        {
            gc_move(PCA_Used.min_one, i);
        }
    }
    nand_erase(PCA_Used.min_one);
    PCA_Used.cnt[PCA_Used.min_one] = 0;
    enqueue(PCA_Empty, PCA_Used.min_one);
    PCA_Empty.size += 1;

    if(curr_pca.fields.page >= page_number)
    {
        curr_pca.fields.block = PCA_Empty.arr[0];
        curr_pca.fields.page = 0;
        dequeue(PCA_Empty);
        PCA_Empty.size -= 1;
    }

    return;
}

static unsigned int get_next_pca()
{
    /*  TODO: seq A, need to change to seq B */
    // Done
	
    if (curr_pca.pca == INVALID_PCA)
    {
        //init
        curr_pca.fields.block = PCA_Empty.arr[0];
        dequeue(PCA_Empty);
        PCA_Empty.size -= 1;
        printf("PCA_Empty size : %d\n", PCA_Empty.size);
        curr_pca.fields.page = 0;
    }

    else if(curr_pca.pca != FULL_PCA) curr_pca.fields.page += 1;

    if (curr_pca.pca == FULL_PCA || curr_pca.fields.page >= page_number)
    {
        if (PCA_Empty.size == 0)
        {
            // ssd is full, no pca can be allocated
            printf("No new PCA!\n");
            curr_pca.pca = FULL_PCA;
            return curr_pca.pca;
        }

        // allocate new block which is empty
        curr_pca.fields.block = PCA_Empty.arr[0];
        curr_pca.fields.page = 0;
        dequeue(PCA_Empty);
        PCA_Empty.size -= 1;
        printf("PCA_Empty size : %d\n", PCA_Empty.size);

        if (PCA_Empty.size == 0)
        {
            // ssd is nearly full, do garbage collection
            printf("=================\n");
            printf("Need to do GC!\n");
            ftl_gc();
        }
    }

    // update PCA_Used to store which nand has been occupied
    PCA_Used.arr[curr_pca.fields.block][curr_pca.fields.page] = true;
    PCA_Used.cnt[curr_pca.fields.block] += 1;

    printf("PCA = page %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);
    return curr_pca.pca;
}

static int ftl_read( char* buf, size_t lba)
{
    PCA_RULE pca;
	pca.pca = L2P[lba];

	if (pca.pca == INVALID_PCA) {
	    //data has not be written, return 0
	    return 0;
	}
	else {
	    return nand_read(buf, pca.pca);
	}
}

static int ftl_write(const char* buf, size_t lba_rnage, size_t lba)
{
    /*  TODO: only basic write case, need to consider other cases */
    // Done

    /**
    if(L2P[lba] != INVALID_PCA)
    {
        printf(" --> Cannot write at the same lba !!!\n");
        return -EINVAL;
    }
    **/
    
    PCA_RULE pca;
    pca.pca = get_next_pca();

    if (nand_write(buf, pca.pca, lba_rnage) > 0)
    {
        L2P[lba] = pca.pca;
        P2L[pca.fields.block * 20 + pca.fields.page] = lba;
        return 512;
    }
    else
    {
        printf(" --> Write fail !!!\n");
        return -EINVAL;
    }
}

static int ssd_file_type(const char* path)
{
    if (strcmp(path, "/") == 0)
    {
        return SSD_ROOT;
    }
    if (strcmp(path, "/" SSD_NAME) == 0)
    {
        return SSD_FILE;
    }
    return SSD_NONE;
}
static int ssd_getattr(const char* path, struct stat* stbuf,
                       struct fuse_file_info* fi)
{
    (void) fi;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = stbuf->st_mtime = time(NULL);
    switch (ssd_file_type(path))
    {
        case SSD_ROOT:
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            break;
        case SSD_FILE:
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            stbuf->st_size = logic_size;
            break;
        case SSD_NONE:
            return -ENOENT;
    }
    return 0;
}
static int ssd_open(const char* path, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_NONE)
    {
        return 0;
    }
    return -ENOENT;
}
static int ssd_do_read(char* buf, size_t size, off_t offset)
{
    int tmp_lba, tmp_lba_range, rst ;
    char* tmp_buf;

    // out of limit
    if ((offset ) >= logic_size)
    {
        return 0;
    }
    if ( size > logic_size - offset)
    {
        //is valid data section
        size = logic_size - offset;
    }

    tmp_lba = offset / 512;
	tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;
    tmp_buf = calloc(tmp_lba_range * 512, sizeof(char));

    for (int i = 0; i < tmp_lba_range; i++) {
        rst = ftl_read(tmp_buf + i * 512, tmp_lba++);
        if ( rst == 0)
        {
            //data has not be written, return empty data
            memset(tmp_buf + i * 512, 0, 512);
        }
        else if (rst < 0 )
        {
            free(tmp_buf);
            return rst;
        }
    }

    memcpy(buf, tmp_buf + offset % 512, size);

    free(tmp_buf);
    return size;
}
static int ssd_read(const char* path, char* buf, size_t size,
                    off_t offset, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_read(buf, size, offset);
}
static int ssd_do_write(const char* buf, size_t size, off_t offset)
{
    /*  TODO: only basic write case, need to consider other cases */
    // Done
	
    int tmp_lba, tmp_lba_range, process_size;
    int idx, curr_size, remain_size, rst;

    host_write_size += size;
    if (ssd_expand(offset + size) != 0)
    {
        return -ENOMEM;
    }

    tmp_lba = offset / 512;                               //27
    offset = offset % 512;                                //176
    tmp_lba_range = (offset + size - 1) / 512 + 1;        //11
    printf("offset : %ld, lba number : %d, range : %d\n", offset, tmp_lba, tmp_lba_range);

    process_size = 0;
    remain_size = size;
    curr_size = 0;

    for (idx = 0; idx < tmp_lba_range; idx++)
    {
        /*  example only align 512, need to implement other cases  */
        if (offset == 0)
        {
            if (remain_size >= 512)
            {
                rst = ftl_write(buf + process_size, 512, tmp_lba + idx);
            }
            else
            {
                rst = ftl_write(buf + process_size, remain_size, tmp_lba + idx);
            }

            if (rst < 0)
            {
                // Write fail
                return -ENOMEM;
            }

            curr_size += rst;
            remain_size -= rst;
            process_size += 512;
        }
        else
        {
            rst = 512 - offset;

            char* tmp_buf;
            tmp_buf = malloc(512 * sizeof(char));
            int read_size;

            read_size = ftl_read(tmp_buf, tmp_lba + idx);
            if(read_size == 0) memset(tmp_buf, 0, 512);
            else
            {
                PCA_RULE pca;
                pca.pca = L2P[tmp_lba+idx];
                PCA_Used.arr[pca.fields.block][pca.fields.page] = false;
                PCA_Used.cnt[pca.fields.block] -= 1;
                P2L[pca.fields.block * 20 + pca.fields.page] = INVALID_PCA;
                L2P[tmp_lba+idx] = INVALID_PCA;
            }

            if (remain_size >= rst)
            {
                memcpy(tmp_buf + offset, buf + process_size, rst);
                rst = ftl_write(tmp_buf, offset + rst, tmp_lba + idx);
            }
            else
            {
                memcpy(tmp_buf + offset, buf + process_size, remain_size);
                if(read_size > offset + remain_size) rst = ftl_write(tmp_buf, read_size, tmp_lba + idx);
                else rst = ftl_write(tmp_buf, offset + remain_size, tmp_lba + idx);
            }

            if (rst < 0)
            {
                // Write fail
                return -ENOMEM;
            }

            curr_size += rst;
            remain_size -= (512 - offset);
            process_size += (512 - offset);
            offset = 0;
        }
    }

    return size;
}
static int ssd_write(const char* path, const char* buf, size_t size,
                     off_t offset, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_write(buf, size, offset);
}
static int ssd_truncate(const char* path, off_t size,
                        struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }

    return ssd_resize(size);
}
static int ssd_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info* fi,
                       enum fuse_readdir_flags flags)
{
    (void) fi;
    (void) offset;
    (void) flags;
    if (ssd_file_type(path) != SSD_ROOT)
    {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, SSD_NAME, NULL, 0, 0);
    return 0;
}
static int ssd_ioctl(const char* path, unsigned int cmd, void* arg,
                     struct fuse_file_info* fi, unsigned int flags, void* data)
{

    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    if (flags & FUSE_IOCTL_COMPAT)
    {
        return -ENOSYS;
    }
    switch (cmd)
    {
        case SSD_GET_LOGIC_SIZE:
            *(size_t*)data = logic_size;
            printf(" --> logic size: %ld\n", logic_size);
            return 0;
        case SSD_GET_PHYSIC_SIZE:
            *(size_t*)data = physic_size;
            printf(" --> physic size: %ld\n", physic_size);
            return 0;
        case SSD_GET_WA:
            *(double*)data = (double)nand_write_size / (double)host_write_size;
            return 0;
    }
    return -EINVAL;
}
static const struct fuse_operations ssd_oper =
{
    .getattr        = ssd_getattr,
    .readdir        = ssd_readdir,
    .truncate       = ssd_truncate,
    .open           = ssd_open,
    .read           = ssd_read,
    .write          = ssd_write,
    .ioctl          = ssd_ioctl,
};
int main(int argc, char* argv[])
{
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
	nand_write_size = 0;
	host_write_size = 0;
    page_number = NAND_SIZE_KB * 1024 / 512;
    curr_pca.pca = INVALID_PCA;
    L2P = malloc(LOGICAL_NAND_NUM * page_number * sizeof(int));
    memset(L2P, INVALID_PCA, sizeof(int)*LOGICAL_NAND_NUM * page_number);
    P2L = malloc(PHYSICAL_NAND_NUM * page_number * sizeof(int));
    memset(P2L, INVALID_PCA, sizeof(int)*PHYSICAL_NAND_NUM * page_number);

    // initialize empty PCA queue
    PCA_Empty.arr = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    for(int i = 0; i < PHYSICAL_NAND_NUM; i++)
    {
        PCA_Empty.arr[i] = i;
    }
    PCA_Empty.size = PHYSICAL_NAND_NUM;

    // initialize PCA usage record
    PCA_Used.arr = malloc(PHYSICAL_NAND_NUM * page_number * sizeof(bool));
    for(int i = 0; i < PHYSICAL_NAND_NUM; i++)
    {
        PCA_Used.arr[i] = malloc(page_number * sizeof(bool));
        memset(PCA_Used.arr[i], false, page_number * sizeof(bool));
    }
    PCA_Used.cnt = malloc(PHYSICAL_NAND_NUM * sizeof(int));
    memset(PCA_Used.cnt, 0, PHYSICAL_NAND_NUM * sizeof(int));
    PCA_Used.min_one = 0;
    PCA_Used.min_two = 1;

    //create nand file
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        FILE* fptr;
        snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, idx);
        fptr = fopen(nand_name, "w");
        if (fptr == NULL)
        {
            printf("open fail");
        }
        fclose(fptr);
    }
    return fuse_main(argc, argv, &ssd_oper, NULL);
}
