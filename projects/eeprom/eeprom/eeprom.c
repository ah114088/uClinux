/*
 * eeprom.c - A loadable kernel module for LPC 17xx EEPROMs.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <mach/clock.h>

/*
 * Driver verbosity level: 0->silent; >0->verbose
 */
static int eeprom_debug = 0;

/*
 * User can change verbosity of the driver
 */
module_param(eeprom_debug, int, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(eeprom_debug, "EEPROM driver verbosity level");

/*
 * Service to print debug messages
 */
#define d_printk(level, fmt, args...)				\
	if (eeprom_debug >= level) printk(KERN_INFO "%s: " fmt,	\
					__func__, ## args)

/*
 * Device major number
 */
static uint eeprom_major = 166;

/*
 * User can change the major number
 */
module_param(eeprom_major, uint, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(eeprom_major, "EEPROM driver major number");

/*
 * Device name
 */
static char *eeprom_name = "eeprom";

/*
 * Device access lock. Only one process can access the driver at a time
 */
static int eeprom_lock = 0;

/*
 * Definitions and prototypes for functions that do the actual work. 
 * Taken from LPCopen v1.03 and adopted.
 */

/* Crystal frequency into device 
   See 'osc_clk' at pg. 21 in LPC 178x/7x User Manual (UM10470.pdf) */
#define CRYSTAL_MAIN_FREQ_IN (24000000)
#define SYSCTL_IRC_FREQ (12000000)

#define LPC_EEPROM_BASE           0x00200080
#define LPC_EEPROM                ((EEPROM_T              *) LPC_EEPROM_BASE)

/*
 * EEPROM registers
 */
typedef struct {                
    u32 CMD;          /* command register */
    u32 ADDR;         /* address register */
    u32 WDATA;        /* write data register */
    u32 RDATA;        /* read data register */
    u32 WSTATE;       /* wait state register */
    u32 CLKDIV;       /* clock divider register */
    u32 PWRDWN;       /* power-down register */
    u32 RESERVED0[975];
    u32 INTENCLR;     /* interrupt enable clear */
    u32 INTENSET;     /* interrupt enable set */
    u32 INTSTAT;      /* interrupt status */
    u32 INTEN;        /* interrupt enable */
    u32 INTSTATCLR;   /* interrupt status clear */
    u32 INTSTATSET;   /* interrupt status set */
} EEPROM_T;


/*
 * EEPROM supports 4032 bytes in 63 pages with 64 bytes per page
 */

#define EEPROM_PAGE_SIZE                64
#define EEPROM_PAGE_NUM                 63

/*
 * defines for command register
 */
#define EEPROM_CMD_8BITS_READ           (0)     /* EEPROM 8-bit read command */
#define EEPROM_CMD_16BITS_READ          (1)     /* EEPROM 16-bit read command */
#define EEPROM_CMD_32BITS_READ          (2)     /* EEPROM 32-bit read command */
#define EEPROM_CMD_8BITS_WRITE          (3)     /* EEPROM 8-bit write command */
#define EEPROM_CMD_16BITS_WRITE         (4)     /* EEPROM 16-bit write command */
#define EEPROM_CMD_32BITS_WRITE         (5)     /* EEPROM 32-bit write command */
#define EEPROM_CMD_ERASE_PRG_PAGE       (6)     /* EEPROM erase/program command */
#define EEPROM_CMD_RDPREFETCH           (1 << 3)/* EEPROM read pre-fetch enable */

/* 
 * defines for interrupt related registers
 */
#define EEPROM_INT_ENDOFRW                 (1 << 26)
#define EEPROM_INT_ENDOFPROG               (1 << 28)
/*
 *
 */
static inline void EEPROM_SetCmd(u32 cmd)
{
    LPC_EEPROM->CMD = cmd;
}

static inline void EEPROM_SetAddr(u32 pageAddr, u32 pageOffset)
{
    LPC_EEPROM->ADDR = (pageAddr << 6) | pageOffset;
}

static inline void EEPROM_WriteData(u32 data)
{
    LPC_EEPROM->WDATA = data;
}

static inline u32 EEPROM_ReadData(void)
{
    return LPC_EEPROM->RDATA;
}

static inline void EEPROM_DisablePowerDown(void)
{
    LPC_EEPROM->PWRDWN = 0;
}

static inline void EEPROM_SetWaitState(u32 ws)
{
    LPC_EEPROM->WSTATE = ws;
}

static inline void EEPROM_ClearIntStatus(u32 mask)
{
    LPC_EEPROM->INTSTATCLR =  mask;
}

static inline u32 EEPROM_GetIntStatus(void)
{
    return LPC_EEPROM->INTSTAT;
}

static void EEPROM_Init(void)
{
    u32 val, cclk;

    EEPROM_DisablePowerDown();

    /* Setup EEPROM timing to 375KHz based on PCLK rate */
	cclk = lpc178x_clock_get(CLOCK_PCLK);
    LPC_EEPROM->CLKDIV = cclk / 375000 - 1;

    /* Setup EEPROM wait states to 15, 35, 35nS */
    val  = ((((cclk / 1000000) * 15) / 1000) + 1);
    val |= (((((cclk / 1000000) * 55) / 1000) + 1) << 8);
    val |= (((((cclk / 1000000) * 35) / 1000) + 1) << 16);
    EEPROM_SetWaitState(val);
}

static void EEPROM_WaitForIntStatus(u32 mask)
{
    u32 status;
    while (1) {
        status = EEPROM_GetIntStatus();
        if ((status & mask) == mask) {
            break;
        }
    }
    EEPROM_ClearIntStatus(mask);
}

/* Read data from non-volatile memory */
static u32 EEPROM_Read(u32 pageOffset, u32 pageAddr, u8 *pData, u32 byteNum)
{
    u32 i;

    EEPROM_ClearIntStatus(EEPROM_INT_ENDOFRW);
    EEPROM_SetAddr(pageAddr, pageOffset);
	EEPROM_SetCmd(EEPROM_CMD_8BITS_READ | EEPROM_CMD_RDPREFETCH);

    /* read and store data in buffer */
    for (i = 0; i < byteNum; i++) {
        pData[i] = EEPROM_ReadData();
        EEPROM_WaitForIntStatus(EEPROM_INT_ENDOFRW);
    }
    return i;
}

/* Write data from page register to non-volatile memory */
static void EEPROM_EraseProgramPage(u16 pageAddr)
{
    EEPROM_ClearIntStatus(EEPROM_CMD_ERASE_PRG_PAGE);
    EEPROM_SetAddr(pageAddr, 0);
    EEPROM_SetCmd(EEPROM_CMD_ERASE_PRG_PAGE);
    EEPROM_WaitForIntStatus(EEPROM_INT_ENDOFPROG);
}

/* Write data to page register */
static u32 EEPROM_WritePageRegister(u16 pageOffset, const u8 *pData, u32 byteNum)
{
    u32 i = 0;

    EEPROM_ClearIntStatus(EEPROM_INT_ENDOFRW);
	EEPROM_SetCmd(EEPROM_CMD_8BITS_WRITE);
    EEPROM_SetAddr(0, pageOffset);

    for (i = 0; i < byteNum; i++) {
        EEPROM_WriteData(pData[i]);
        EEPROM_WaitForIntStatus(EEPROM_INT_ENDOFRW);
    }

    return i;
}


/*
 * Device open
 */
static int eeprom_open(struct inode *inode, struct file *file)
{
	int ret = 0;

	/*
	 * One process at a time
	 */
	if (eeprom_lock ++ > 0) {
		ret = -EBUSY;
		goto Done;
	}
 
	/*
 	 * Increment the module use counter
 	 */
	try_module_get(THIS_MODULE);

Done:
	d_printk(2, "lock=%d\n", eeprom_lock);
	return ret;
}

/*
 * Device close
 */
static int eeprom_release(struct inode *inode, struct file *file)
{
	/*
 	 * Release device
 	 */
	eeprom_lock = 0;

	/*
 	 * Decrement module use counter
 	 */
	module_put(THIS_MODULE);

	d_printk(2, "lock=%d\n", eeprom_lock);
	return 0;
}

loff_t eeprom_llseek(struct file *filp, loff_t offset, int whence)
{
    loff_t newpos;

	switch (whence) {
	case SEEK_SET:
		newpos = offset;
		break;
	
	case SEEK_CUR:
        newpos = filp->f_pos + offset;
		break;

    case SEEK_END:
        newpos = EEPROM_PAGE_SIZE * EEPROM_PAGE_NUM + offset;
        break;

    default:
        return -EINVAL;
    }

    if (newpos < 0) return -EINVAL;
    filp->f_pos = newpos;
    return newpos;
}

/* 
 * Device read
 */
static ssize_t eeprom_read(struct file *filp, char *buffer,
			 size_t length, loff_t * offset)
{
	int ret = 0;
	size_t remaining, to_read, read_bytes, page_offset;
    u16 page;

	/*
 	 * Check that the user has supplied a valid buffer
 	 */
	if (! access_ok(0, buffer, length)) {
		ret = -EINVAL;
		goto Done;
	}

    /* EEPROM has a fixed size. May not go beyond end */
    remaining = (EEPROM_PAGE_SIZE*EEPROM_PAGE_NUM) - *offset;
    if (length > remaining)
		length = remaining;

    /* return EOF condition if all was read */
    if (length == 0) {
		ret = 0;
		goto Done;
	}

	for (to_read = length; to_read > 0; to_read -= read_bytes) {
		page = *offset >> 6; 
		page_offset = *offset & (EEPROM_PAGE_SIZE-1);
        
		if (to_read > (EEPROM_PAGE_SIZE - page_offset))
			read_bytes = EEPROM_PAGE_SIZE - page_offset; 
        else
			read_bytes = to_read;
			
        EEPROM_Read(page_offset, page, buffer, read_bytes);
		*offset += read_bytes;
    } 

	ret = length;
Done:
	d_printk(3, "length=%d,ret=%d\n", length, ret);
	return ret;
}

/* 
 * Device write
 */
static ssize_t eeprom_write(struct file *filp, const char *buffer,
			  size_t length, loff_t * offset)
{
	int ret = 0;
	size_t remaining, to_write, write_bytes, page_offset;
    u16 page;

	/*
	* Check that the user has supplied a valid buffer
	*/
	if (! access_ok(0, buffer, length)) {
		ret = -EINVAL;
		goto Done;
	}

    /* EEPROM has a fixed size. May not go beyond end */
    remaining = (EEPROM_PAGE_SIZE*EEPROM_PAGE_NUM) - *offset;
    if (length > remaining)
		length = remaining;

    /* return EOF condition if all was read */
    if (length == 0) {
		ret = 0;
		goto Done;
	}

	for (to_write = length; to_write > 0; to_write -= write_bytes) {
		page = *offset >> 6; 
		page_offset = *offset & (EEPROM_PAGE_SIZE-1);
        
		if (to_write > (EEPROM_PAGE_SIZE - page_offset))
			write_bytes = EEPROM_PAGE_SIZE - page_offset; 
        else
			write_bytes = to_write;
			
		EEPROM_WritePageRegister(page_offset, buffer, write_bytes);
		EEPROM_EraseProgramPage(page);
		*offset += write_bytes;
		buffer += write_bytes;
    } 
    
    ret = length;

Done:
	d_printk(3, "length=%d\n", length);
	return ret;
}

/*
 * Device operations
 */
static struct file_operations eeprom_fops = {
	.read = eeprom_read,
	.write = eeprom_write,
	.llseek = eeprom_llseek,
	.open = eeprom_open,
	.release = eeprom_release
};

static int __init eeprom_init_module(void)
{
	int ret = 0;

	/*
 	 * check that the user has supplied a correct major number
 	 */
	if (eeprom_major == 0) {
		printk(KERN_ALERT "%s: eeprom_major can't be 0\n", __func__);
		ret = -EINVAL;
		goto Done;
	}

	/*
 	 * Register device
 	 */
	ret = register_chrdev(eeprom_major, eeprom_name, &eeprom_fops);
	if (ret < 0) {
		printk(KERN_ALERT "%s: registering device %s with major %d "
				  "failed with %d\n",
		       __func__, eeprom_name, eeprom_major, ret);
		goto Done;
	}

	EEPROM_Init();
	
Done:
	d_printk(1, "name=%s,major=%d\n", eeprom_name, eeprom_major);

	return ret;
}
static void __exit eeprom_cleanup_module(void)
{
	/*
	 * Unregister device
	 */
	unregister_chrdev(eeprom_major, eeprom_name);

	d_printk(1, "%s\n", "clean-up successful");
}

module_init(eeprom_init_module);
module_exit(eeprom_cleanup_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andreas Haas, ah114088@gmx.de");
MODULE_DESCRIPTION("EEPROM device driver");
