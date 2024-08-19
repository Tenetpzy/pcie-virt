#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>

#define MAX_RAID_NUM 4
#define RAID_DEVICE_NAME "gpuRAID"
#define RAID_OPEN_MODE (FMODE_READ | FMODE_WRITE | FMODE_EXCL)

static int raid_num = 0;
static char* raid[MAX_RAID_NUM] = {NULL};

module_param_array(raid, charp, &raid_num, S_IRUSR);
MODULE_PARM_DESC(raid, "the disk files array in RAID(e.g. raid=\"/dev/sda1\",\"/dev/sda2\")");

static blk_qc_t raid_make_request(struct request_queue *queue, struct bio *bio);

struct RaidBlockDevice
{
    struct request_queue *queue;
    struct gendisk *gd;
    // struct bio_set bio_pool;
    int major;

    struct block_device *devices[MAX_RAID_NUM];  // RAID下层控制的每一个块设备(或分区)
    int avail_device_num;  // 加载时指定的下层设备数
    int data_device_num;  // 用于存储数据的盘数(avail_device_num - 1，最后一个用于校验盘)
    sector_t sector_num;  // 下层设备的统一的扇区数

    // TODO: reference to PCIE device BAR
} raid_dev;

struct block_device_operations raid_blk_ops = {};

/* 
检查: 下层的每个设备是否都存在且配置相同
初始化：初始化raid_dev的devices成员，指向对应的下层设备
*/
static int check_init_raid(struct RaidBlockDevice *raid_dev)
{
    int ret = 0;
    struct block_device *first;
    sector_t sector_num;
    int cur;
    int i;
    if (raid_num < 3 || raid_num > 4)
    {
        pr_err("raid needs at least 3 and no more than 4 devices, current: %d\n", raid_num);
        return -EINVAL;
    }

    // 以首个设备的扇区数为准
    // note: FMODE_EXCL: 以独占模式打开，holder参数相当于拥有者的key，同一拥有者可以递归打开
    first = blkdev_get_by_path(raid[0], RAID_OPEN_MODE, raid_dev);
    if (IS_ERR(first))
    {
        pr_err("failed to open %s\n", raid[0]);
        return PTR_ERR(first);
    }
    pr_info("open %s success, sector number: %llu\n", raid[0], first->bd_part->nr_sects);
    raid_dev->devices[0] = first;
    sector_num = first->bd_part->nr_sects;

    // 检查其它盘的扇区数是否一致，一致则初始化devices数组
    cur = 1;
    for (; cur < raid_num; ++cur)
    {
        struct block_device *bd = blkdev_get_by_path(raid[cur], RAID_OPEN_MODE, raid_dev);
        if (IS_ERR(bd))
        {
            pr_err("failed to open %s\n", raid[cur]);
            ret = PTR_ERR(bd);
            goto ERR_OPEN;
        }
        raid_dev->devices[cur] = bd;
        pr_info("open %s success, sector number: %llu\n", raid[cur], bd->bd_part->nr_sects);
        if (bd->bd_part->nr_sects != sector_num)
        {
            pr_err("sector num doesn't match\n");
            ++cur;  // 当前打开的盘需要释放
            goto ERR_OPEN;
        }
    }
    
    // ret = bioset_init(&raid_dev->bio_pool, BIO_POOL_SIZE, 0, 0);
    // if (ret)
    // {
    //     pr_err("alloc bio pool failed.\n");
    //     goto ERR_OPEN;
    // }
    raid_dev->avail_device_num = raid_num;
    raid_dev->data_device_num = raid_num - 1;
    raid_dev->sector_num = sector_num * raid_dev->data_device_num;

    // TODO: 初始化到PCIE虚拟设备的引用

    return ret;

ERR_OPEN:
    for (i = 0; i < cur; ++i)
        blkdev_put(raid_dev->devices[i], RAID_OPEN_MODE);
    return ret;
}

static int create_block_device(struct RaidBlockDevice *raid_dev)
{
    int ret = 0;
    struct gendisk *gd;
    ret = register_blkdev(0, RAID_DEVICE_NAME);
    if (ret < 0)
    {
        pr_err("failed to alloc major number\n");
        goto ERR_OUT;
    }
    raid_dev->major = ret;
    
    gd = alloc_disk(2);  // 目前只支持1个分区
    if (!gd)
    {
        pr_err("failed to alloc disk\n");
        ret = -ENOMEM;
        goto ERR_REGISER_MAJOR;
    }

    raid_dev->queue = blk_alloc_queue(GFP_KERNEL);
    if (!raid_dev->queue)
    {
        pr_err("failed to alloc queue\n");
        ret = -ENOMEM;
        goto ERR_ALLOC_DISK;
    }
    raid_dev->queue->queuedata = raid_dev;
    blk_queue_make_request(raid_dev->queue, raid_make_request);  // 绕过内核请求队列，直接处理bio

    gd->major = raid_dev->major;
    gd->first_minor = 0;  // 0还是1?
    gd->fops = &raid_blk_ops;
    gd->queue = raid_dev->queue;
    gd->private_data = raid_dev;
    snprintf(gd->disk_name, 32, RAID_DEVICE_NAME);
    set_capacity(gd, raid_dev->sector_num);
    raid_dev->gd = gd;

    add_disk(gd);

    return 0;

ERR_ALLOC_DISK:
    del_gendisk(gd);
ERR_REGISER_MAJOR:
    unregister_blkdev(raid_dev->major, RAID_DEVICE_NAME);
ERR_OUT:
    return ret; 
}

static int __init raid_block_init(void)
{
    int ret = 0;
    int i;
    ret = check_init_raid(&raid_dev);
    if (ret != 0)
        return ret;
    ret = create_block_device(&raid_dev);
    if (ret != 0)
        for (i = 0; i < raid_dev.avail_device_num; ++i)
            blkdev_put(raid_dev.devices[i], RAID_OPEN_MODE);
    return ret;
}

static void __exit raid_block_exit(void)
{
    int i;
    del_gendisk(raid_dev.gd);
    blk_cleanup_queue(raid_dev.queue);
    unregister_blkdev(raid_dev.major, RAID_DEVICE_NAME);
    // bioset_exit(&raid_dev.bio_pool);
    for (i = 0; i < raid_dev.avail_device_num; ++i)
        blkdev_put(raid_dev.devices[i], RAID_OPEN_MODE);
}

static void raid_handle_read_req(struct RaidBlockDevice *raid_dev, struct bio *bio)
{
    /*
    目前假设RAID中数据按扇区划分和排列，第x个扇区保存在下标为 x % data_device_num 的盘中
    因此不断从bio中以一个扇区为单位进行分裂出子bio，并使用bio_chain将子bio与bio关联
    在使用bio_chain后，bio的回调函数只会在所有子bio和它本身完成后才被调用，且子bio由内核负责释放
    于是我们把所有子bio和最后剩下的单扇区bio分别再次提交给下层的盘即可
    */
    int loop = true;
    while (loop)
    {
        struct bio *bio_to_submit = bio;
        sector_t cur_sector;
        int target_dev;
        if (likely(bio_sectors(bio) > 1))
        {
            struct bio *child_bio = bio_split(bio, 1, GFP_NOIO, &raid_dev->queue->bio_split);
            if (!child_bio)  // TODO: 部分读?
            {
                pr_err("failed to alloc child bio");
                bio_io_error(bio);
                return;
            }

            // 按照bio_chain的要求，它俩必须为NULL，由chain内部管理
            child_bio->bi_private = NULL;
            child_bio->bi_end_io = NULL;
            bio_chain(child_bio, bio);

            bio_to_submit = child_bio;
            loop = true;
        }
        else
            loop = false;

        cur_sector = bio_to_submit->bi_iter.bi_sector;
        BUG_ON(bio_sectors(bio_to_submit) != 1);
        target_dev = cur_sector % raid_dev->data_device_num;
        bio_to_submit->bi_iter.bi_sector = cur_sector / raid_dev->data_device_num;
        bio_set_dev(bio_to_submit, raid_dev->devices[target_dev]);

        pr_info("submit read bio to %s, target sector: %llu, sector in target device: %llu\n", raid[target_dev], cur_sector, 
            cur_sector / raid_dev->data_device_num);

        submit_bio(bio_to_submit);
    }
}

static void raid_handle_write_req(struct RaidBlockDevice *raid_dev, struct bio *bio)
{
    int loop = true;
    while (loop)
    {
        struct bio *bio_to_submit = bio;
        sector_t cur_sector;
        int target_dev;
        if (likely(bio_sectors(bio) > 1))
        {
            struct bio *child_bio = bio_split(bio, 1, GFP_NOIO, &raid_dev->queue->bio_split);
            if (!child_bio)
            {
                pr_err("failed to alloc child bio");
                bio_io_error(bio);
                return;
            }

            // 按照bio_chain的要求，它俩必须为NULL，由chain内部管理
            child_bio->bi_private = NULL;
            child_bio->bi_end_io = NULL;
            bio_chain(child_bio, bio);

            bio_to_submit = child_bio;
            loop = true;
        }
        else
            loop = false;

        cur_sector = bio_to_submit->bi_iter.bi_sector;
        BUG_ON(bio_sectors(bio_to_submit) != 1);
        target_dev = cur_sector % raid_dev->data_device_num;
        bio_to_submit->bi_iter.bi_sector = cur_sector / raid_dev->data_device_num;
        bio_set_dev(bio_to_submit, raid_dev->devices[target_dev]);

        pr_info("submit write bio to %s, target sector: %llu, sector in target device: %llu\n", raid[target_dev], cur_sector, 
            cur_sector / raid_dev->data_device_num);

        submit_bio(bio_to_submit);

        // TODO: 告知PCIE设备处理校验和
    }
}

static blk_qc_t raid_make_request(struct request_queue *queue, struct bio *bio)
{
    struct RaidBlockDevice *raid_dev = queue->queuedata;
    if (bio_data_dir(bio) == READ)
        raid_handle_read_req(raid_dev, bio);
    else
        raid_handle_write_req(raid_dev, bio);
    return BLK_QC_T_NONE;
}

module_init(raid_block_init);
module_exit(raid_block_exit);

MODULE_LICENSE("GPL");