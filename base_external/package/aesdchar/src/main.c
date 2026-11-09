/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include <linux/container_of.h>
#include <linux/slab.h>
#include "aesd-circular-buffer.h"
#include "aesdchar.h"
#include "aesd_ioctl.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("NMT81"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

static int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    struct aesd_dev *tmp;
    tmp = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = tmp;

    return 0;
}

static int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
    return 0;
}

static ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    struct aesd_dev *dev = (struct aesd_dev*)filp->private_data;
    struct aesd_buffer_entry* entry = NULL;
    size_t bytes_recv = 0;
    size_t offset = 0;
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle read
     */
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->rb, (size_t)*f_pos, &offset);
    if (entry){
        if (count > (entry->size - offset)) bytes_recv = entry->size - offset;
        else bytes_recv = count;
        if (copy_to_user(buf, &entry->buffptr[offset], bytes_recv) != 0) {
            retval = -EFAULT;
            goto out;
        }
        *f_pos += bytes_recv;
        retval = bytes_recv;
    } else {
        retval = 0;
    }
out:
    mutex_unlock(&dev->lock);
    return retval;
}

static ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    char *tmbuf = NULL;
    char *rtnptr = NULL;
    struct aesd_dev *dev = (struct aesd_dev*)filp->private_data;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle write
     */
    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }
    if (dev->entry.buffptr == NULL) {
        tmbuf = kmalloc(count, GFP_KERNEL);
        if (!tmbuf){
            retval = -ENOMEM;
            goto out;
        }
        if (copy_from_user(tmbuf, buf, count) != 0) {
            kfree(tmbuf);
            retval = -EFAULT;
            goto out;
        }
    } else {
        tmbuf = krealloc(dev->entry.buffptr, dev->entry.size + count, GFP_KERNEL);
        if (!tmbuf){
            kfree(dev->entry.buffptr);
            retval = -ENOMEM;
            goto out;
        }
        if (copy_from_user(&tmbuf[dev->entry.size], buf, count) != 0) {
            kfree(tmbuf);
            retval = -EFAULT;
            goto out;
        }
    }
    dev->entry.buffptr = tmbuf;
    dev->entry.size += count;
    *f_pos = dev->entry.size;
    if (dev->entry.buffptr[dev->entry.size -1] == '\n') {
        rtnptr = (char*)aesd_circular_buffer_add_entry(&dev->rb,&dev->entry);
        if (rtnptr) {
            kfree(rtnptr);
        }
        memset(&dev->entry,0,sizeof(struct aesd_buffer_entry));
    }
    retval = count;
out:
    mutex_unlock(&dev->lock);
    return retval;
}

static loff_t aesd_llseek(struct file *file, loff_t offset, int whence)
{
    loff_t new_pos;
    size_t max_len = 0;
    struct aesd_dev *dev = (struct aesd_dev*)file->private_data;
    uint8_t tmp = 0;
    ssize_t retval = 0;

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }
    tmp = dev->rb.out_offs;
    if ((!dev->rb.full)&&(dev->rb.in_offs == tmp)) {           /* empty */
        max_len = 0;
    } else {
        do {
            max_len += dev->rb.entry[tmp].size;
            tmp = (tmp + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        } while (tmp != dev->rb.in_offs);
    }

    switch (whence) {
        case SEEK_SET:
            new_pos = offset;
            break;
        case SEEK_CUR:
            new_pos = file->f_pos + offset;
            break;
        case SEEK_END:
            new_pos = (max_len ? max_len - 1 : 0) + offset;
            break;
        default:
            retval = -EINVAL;
            goto out;
    }
    
    if ((new_pos < 0) || ((new_pos > 0) && (new_pos >= max_len))) {
        retval = -EINVAL;
        goto out;
    }
    file->f_pos = new_pos;
    retval = new_pos;
out:
    mutex_unlock(&dev->lock);
    return retval;
}

static long aesd_adjust_file_offset (struct file *filp, unsigned int write_cmd, unsigned int write_cmd_offset)
{
    struct aesd_dev *dev = (struct aesd_dev*)filp->private_data;
    ssize_t retval = 0;
    size_t offset = 0;
    uint8_t tmp, cmd_no, i;

    if (mutex_lock_interruptible(&dev->lock)) {
        return -ERESTARTSYS;
    }
    if ((!dev->rb.full)&&(dev->rb.in_offs == dev->rb.out_offs)) {           /* empty */
        retval = -EINVAL;
        goto out;
    } else {
        tmp = dev->rb.out_offs;
        cmd_no = 0;
        do {
            cmd_no++;
            tmp = (tmp + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        } while (tmp != dev->rb.in_offs);
        if (write_cmd >= cmd_no) {
            retval = -EINVAL;
            goto out;
        } else {
            tmp = dev->rb.out_offs;
            for (i = 0; i < write_cmd; i++)
            {
                offset += dev->rb.entry[tmp].size;
                tmp = (tmp + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
            }
            if (write_cmd_offset >= dev->rb.entry[tmp].size) {
                retval = -EINVAL;
                goto out;                
            } else {
                offset += write_cmd_offset;
                filp->f_pos = offset;
                retval = 0;
            }
        }
    }
out:
    mutex_unlock(&dev->lock);
    return retval;
}

static long aesd_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    ssize_t retval = 0;

    switch(cmd) {
        case AESDCHAR_IOCSEEKTO:
            struct aesd_seekto seekto;
            if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)) != 0) {
                retval = EFAULT;
            } else {
                retval = aesd_adjust_file_offset(file, seekto.write_cmd, seekto.write_cmd_offset);
            }
            break;

        default:
            retval = -ENOTTY;
            break;
    }
    return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .llseek =   aesd_llseek,
    .unlocked_ioctl = aesd_unlocked_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



static int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    mutex_init(&aesd_device.lock);
    
    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

static void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */
    for(int i=0; i<AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED; i++){
        if (aesd_device.rb.entry[i].buffptr) {kfree(aesd_device.rb.entry[i].buffptr);}
    }

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
