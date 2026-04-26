/**
 * Raspberry Pi Linux Kernel module derived from
 * https://github.com/raspberrypi/linux/blob/rpi-5.15.y/drivers/char/broadcom/bcm2835-gpiomem.c
 *
 * Copyright (c) 2015, Raspberry Pi (Trading) Ltd.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the above-listed copyright holders may not be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2, as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/cdev.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pagemap.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/version.h>

// periphmem

#define DEVICE_NAME "periphmem"
#define DRIVER_NAME "periphmem-bcm2835"
#define DEVICE_MINOR 0

struct bcm2835_periphmem_instance {
  unsigned long peripherals_addr;
  unsigned long peripherals_end;
  struct device *dev;
};

static struct cdev bcm2835_periphmem_cdev;
static dev_t bcm2835_periphmem_devid;
static struct class *bcm2835_periphmem_class;
static struct device *bcm2835_periphmem_dev;
static struct bcm2835_periphmem_instance *inst;

/****************************************************************************
 *
 *   pcm mem chardev file ops
 *
 ***************************************************************************/

const unsigned long kClockManagerOffset = 0x00101000;
const unsigned long kGPIOOffset = 0x00200000;
const unsigned long kPCMOffset = 0x00203000;
const unsigned long kPWMOffset = 0x0020c000;

/*

PAGE_SIZE
*/

static int bcm2835_periphmem_open(struct inode *inode, struct file *file) {
  int dev = iminor(inode);
  int ret = 0;

  if (dev != DEVICE_MINOR) {
    dev_err(inst->dev, "Unknown minor device: %d", dev);
    ret = -ENXIO;
  }
  return ret;
}

static int bcm2835_periphmem_release(struct inode *inode, struct file *file) {
  int dev = iminor(inode);
  int ret = 0;

  if (dev != DEVICE_MINOR) {
    dev_err(inst->dev, "Unknown minor device %d", dev);
    ret = -ENXIO;
  }
  return ret;
}

static const struct vm_operations_struct bcm2835_periphmem_vm_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
    .access = generic_access_phys
#endif
};

static bool test_in_peripheral(unsigned long peripheral_offset,
                               unsigned long start_offset,
                               unsigned long end_offset) {
  return (start_offset >= peripheral_offset) &&
         (end_offset <= (peripheral_offset + PAGE_SIZE));
}

static int bcm2835_periphmem_mmap(struct file *file,
                                  struct vm_area_struct *vma) {
  unsigned long start_addr = vma->vm_pgoff << PAGE_SHIFT;
  unsigned long size = vma->vm_end - vma->vm_start;
  unsigned long end_addr = start_addr + size;

  // Only map locations in the I/O peripherals block.
  if (start_addr < inst->peripherals_addr || end_addr > inst->peripherals_end) {
    return -EINVAL;
  }

  unsigned long start_off = start_addr - inst->peripherals_addr;
  unsigned long end_off = end_addr - inst->peripherals_addr;

  // Many peripherals are risky to expose to a process so further restrict
  // mappings to some subset of fairly safe.
  if (!test_in_peripheral(kClockManagerOffset, start_off, end_off) &&
      !test_in_peripheral(kGPIOOffset, start_off, end_off) &&
      test_in_peripheral(kPCMOffset, start_off, end_off) &&
      test_in_peripheral(kPWMOffset, start_off, end_off)) {
    return -EINVAL;
  }

  vma->vm_page_prot =
      phys_mem_access_prot(file, vma->vm_pgoff, size, vma->vm_page_prot);
  vma->vm_ops = &bcm2835_periphmem_vm_ops;
  if (remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, size,
                      vma->vm_page_prot)) {
    return -EAGAIN;
  }
  return 0;
}

static const struct file_operations bcm2835_periphmem_fops = {
    .owner = THIS_MODULE,
    .open = bcm2835_periphmem_open,
    .release = bcm2835_periphmem_release,
    .mmap = bcm2835_periphmem_mmap,
};

/****************************************************************************
 *
 *   Probe and remove functions
 *
 ***************************************************************************/

static int bcm2835_periphmem_probe(struct platform_device *pdev) {
  int err;
  void *ptr_err;
  struct device *dev = &pdev->dev;
  struct resource *ioresource;

  /* Allocate buffers and instance data */

  inst = kzalloc(sizeof(struct bcm2835_periphmem_instance), GFP_KERNEL);

  if (!inst) {
    err = -ENOMEM;
    goto failed_inst_alloc;
  }

  inst->dev = dev;

  ioresource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
  if (ioresource) {
    inst->peripherals_addr = ioresource->start;
    inst->peripherals_end = ioresource->end;
  } else {
    dev_err(inst->dev, "failed to get IO resource");
    err = -ENOENT;
    goto failed_get_resource;
  }

  /* Create character device entries */

  err = alloc_chrdev_region(&bcm2835_periphmem_devid, DEVICE_MINOR, 1,
                            DEVICE_NAME);
  if (err != 0) {
    dev_err(inst->dev, "unable to allocate device number");
    goto failed_alloc_chrdev;
  }
  cdev_init(&bcm2835_periphmem_cdev, &bcm2835_periphmem_fops);
  bcm2835_periphmem_cdev.owner = THIS_MODULE;
  err = cdev_add(&bcm2835_periphmem_cdev, bcm2835_periphmem_devid, 1);
  if (err != 0) {
    dev_err(inst->dev, "unable to register device");
    goto failed_cdev_add;
  }

  /* Create sysfs entries */

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
  bcm2835_periphmem_class = class_create(THIS_MODULE, DEVICE_NAME);
#else
  bcm2835_periphmem_class = class_create(DEVICE_NAME);
#endif
  ptr_err = bcm2835_periphmem_class;
  if (IS_ERR(ptr_err)) goto failed_class_create;

  bcm2835_periphmem_dev =
      device_create(bcm2835_periphmem_class, NULL, bcm2835_periphmem_devid,
                    NULL, DEVICE_NAME);
  ptr_err = bcm2835_periphmem_dev;
  if (IS_ERR(ptr_err)) goto failed_device_create;

  dev_info(inst->dev, "Initialised: Registers at 0x%08lx",
           inst->peripherals_addr);

  return 0;

failed_device_create:
  class_destroy(bcm2835_periphmem_class);
failed_class_create:
  cdev_del(&bcm2835_periphmem_cdev);
  err = PTR_ERR(ptr_err);
failed_cdev_add:
  unregister_chrdev_region(bcm2835_periphmem_devid, 1);
failed_alloc_chrdev:
failed_get_resource:
  kfree(inst);
failed_inst_alloc:
  dev_err(inst->dev, "could not load bcm2835_periphmem");
  return err;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 10, 0)
static int bcm2835_periphmem_remove(struct platform_device *pdev) {
#else
static void bcm2835_periphmem_remove(struct platform_device *pdev) {
#endif
  struct device *dev = inst->dev;

  kfree(inst);
  device_destroy(bcm2835_periphmem_class, bcm2835_periphmem_devid);
  class_destroy(bcm2835_periphmem_class);
  cdev_del(&bcm2835_periphmem_cdev);
  unregister_chrdev_region(bcm2835_periphmem_devid, 1);

  dev_info(dev, "Peripheral mem driver removed - OK");
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 10, 0)
  return 0;
#endif
}

/****************************************************************************
 *
 *   Register the driver with device tree
 *
 ***************************************************************************/

static const struct of_device_id bcm2835_periphmem_of_match[] = {
    {
        .compatible = "brcm,bcm2835-periphmem",
    },
    {/* sentinel */},
};

MODULE_DEVICE_TABLE(of, bcm2835_periphmem_of_match);

static struct platform_driver bcm2835_periphmem_driver = {
    .probe = bcm2835_periphmem_probe,
    .remove = bcm2835_periphmem_remove,
    .driver =
        {
            .name = DRIVER_NAME,
            .owner = THIS_MODULE,
            .of_match_table = bcm2835_periphmem_of_match,
        },
};

module_platform_driver(bcm2835_periphmem_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("periphmem driver for accessing peripherals from userspace");