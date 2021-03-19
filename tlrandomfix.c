/*
 * tlrandom.c
 * ver. 2.3
 *
 */

/*
 * TL100/TL200 device driver - 2.3
 *
 * Copyright (C) 2014-2016 TectroLabs, http://tectrolabs.com
 *
 * This is a 'tlrandom' kernel module that registers a character device
 * for supplying true random bytes generated by TL100 and TL200 hardware
 * random number generators.
 *
 * Once the module is successfully built with 'make', it should be loaded
 * into the kernel by running the ins-tlrandom.sh script:
 * sudo ./ins-tlrandom.sh
 *
 * After the module is successfully loaded by the kernel, the random bytes
 * will be available for download from the /dev/tlrandom device.
 *
 * It can be used to feed the 'rngd' daemon with random data generated
 * by a TL100 or TL200 device using the following command:
 * sudo rngd -r /dev/tlrandom
 *
 * Alternatively you can download the random byte stream into a file using
 * the following command:
 * dd if=/dev/tlrandom of=download.bin bs=100 count=120000
 *
 * You can change the 'nod' name to something other than /dev/tlrandom
 * (read the ins-tlrandom.sh for notes).
 *
 * The module will automatically detect when a TL100 or TL200 device is plugged
 * in or unplugged from any USB port.
 *
 * Please note that you will not be able to use 'getrnd' utility with a TL100
 * or TL200 device when the device is in use by the 'tlrandom' module.
 *
 * To verify if the 'tlrandom' module has successfully detected the TL device,
 * simply check the kernel system logs, you should see a log that should
 * look similar to this:
 *
 *   ------------ -----------------------------
 *   -- TL200/100 device connected and ready --
 *   ------------- ----------------------------
 *
 * Currently the 'tlrandom' module can only use one TL device at a time.
 *
 */

#include "tlrandom.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrian Belinski");
MODULE_DESCRIPTION("A module that registers a device for supplying true random bytes generated by Hardware RNG suchs as TL100 or TL200");
MODULE_VERSION("2.3");

/**
 * A function to handle the event when the expected USB device is plugged in or connected
 *
 * @param struct usb_interface *interface - pointer to the usb_interface structure associated with the device
 * @param const struct usb_device_id *id -  pointer to the usb_device_id
 * @return 0 - successfully, otherwise the error code (a negative number)
 *
 */
static int usb_probe(struct usb_interface *interface, const struct usb_device_id *id) {
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	int i;
	size_t buffer_size;
	int retval = SUCCESS;

	if(mutex_lock_killable(&dataOpLock) != SUCCESS) {
		printk(KERN_ALERT "Could not lock the mutex\n");
		return -EPERM;
	}

	if (isEntropySrcRdy || usbData != NULL) {
		printk(KERN_INFO "A TL USB device already registered\n");
		mutex_unlock(&dataOpLock);
		return -EPERM;
	}

	if (isShutDown) {
		printk(KERN_INFO "Cannot register USB device (%04X:%04X) while module is being removed from the kernel\n", id->idVendor, id->idProduct);
		mutex_unlock(&dataOpLock);
		return -EPERM;
	}

	iface_desc = interface->cur_altsetting;


	usbData = kmalloc(sizeof(struct usb_data), GFP_KERNEL);
	if (usbData == NULL) {
		printk(KERN_ALERT "Out of memory\n");
		mutex_unlock(&dataOpLock);
		return -ENOMEM;
	}
	memset(usbData, 0x00, sizeof (struct usb_data));

	usbData->udev = usb_get_dev(interface_to_usbdev(interface));
	usbData->interface = interface;


	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i].desc;

		if (!usbData->bulk_in_endpointAddr &&
		    (endpoint->bEndpointAddress & USB_DIR_IN) &&
		    ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
					== USB_ENDPOINT_XFER_BULK)) {
			buffer_size = endpoint->wMaxPacketSize;
			usbData->bulk_in_size = buffer_size;
			usbData->bulk_in_endpointAddr = endpoint->bEndpointAddress;
			usbData->bulk_in_buffer = kmalloc(USB_BUFFER_SIZE, GFP_KERNEL);
			if (usbData->bulk_in_buffer == NULL) {
				printk(KERN_ALERT "Could not allocate memory for bulk_in_buffer");
				retval = -ENOMEM;
				break;
			}
		}

		if (!usbData->bulk_out_endpointAddr &&
		    !(endpoint->bEndpointAddress & USB_DIR_IN) &&
		    ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
					== USB_ENDPOINT_XFER_BULK)) {
			buffer_size = endpoint->wMaxPacketSize;
			usbData->bulk_out_endpointAddr = endpoint->bEndpointAddress;
			usbData->bulk_out_buffer = kmalloc(buffer_size, GFP_KERNEL);
			if (usbData->bulk_out_buffer == NULL) {
				printk(KERN_ALERT "Could not allocate memory for bulk_out_buffer");
				retval = -ENOMEM;
				break;
			}
		}
	}

	if (retval == SUCCESS && !(usbData->bulk_in_endpointAddr && usbData->bulk_out_endpointAddr)) {
		printk(KERN_INFO "Could not find both bulk-in and bulk-out endpoints");
		retval = -EPERM;
	}

	if (retval != SUCCESS) {
		clean_up_usb();
	} else {
		printk(KERN_INFO "------------------------------------------\n");
		printk(KERN_INFO "-- TL200/100 device connected and ready --\n");
		printk(KERN_INFO "------------------------------------------\n");
		#ifdef inDebugMode
		printk(KERN_INFO "Device is using IN bulk address %02X, OUT bulk address %02X, bulk IN size: %d\n", usbData->bulk_in_endpointAddr, usbData->bulk_out_endpointAddr, (int)usbData->bulk_in_size);
		#endif
		isEntropySrcRdy = true;
	}

	mutex_unlock(&dataOpLock);
	return retval;
}
/**
 * A function to handle the event when the USB device is unplugged in or disconnected
 *
 * @param struct usb_interface *interface - pointer to the usb_interface structure associated with the device
 *
 */
static void usb_disconnect(struct usb_interface *interface) {
	if(mutex_lock_killable(&dataOpLock) != SUCCESS) {
		printk(KERN_INFO "Could not lock the mutex\n");
	}

	isEntropySrcRdy = false;
	clean_up_usb();
	printk(KERN_INFO "USB device disconnected\n");
	mutex_unlock(&dataOpLock);
}

/**
 * A function to clean-up the USB allocated resources
 *
 *
 */

static void clean_up_usb(void) {
	if (usbData != NULL) {
		if ( usbData->bulk_out_buffer != NULL) {
			kfree(usbData->bulk_out_buffer);
			usbData->bulk_out_buffer = NULL;
		}
		if ( usbData->bulk_in_buffer != NULL) {
			kfree(usbData->bulk_in_buffer);
			usbData->bulk_in_buffer = NULL;
		}
		kfree(usbData);
		usbData = NULL;
	}
}

/**
 * A function to handle the event when device is open
 *
 * @param struct inode *inode - pointer to the inode structure of the caller
 * @param struct file *file -  pointer to the file structure of the caller
 * @return 0 - successfully, otherwise the error code (a negative number)
 *
 */
static int device_open(struct inode *inode, struct file *file)
{
	int status = SUCCESS;
	unsigned int mj = imajor(inode);
	unsigned int mn = iminor(inode);

	if (mj != major || mn != minor) {
		printk(KERN_ALERT "No device found with major=%d and minor=%d\n",	mj, mn);
		return -ENODEV;
	}

	if (!isEntropySrcRdy || isShutDown) {
		status = -ENODATA;
	}

	return status;
}

/**
 * A function to handle the event when device is closed
 *
 * @param struct inode *inode - pointer to the inode structure of the caller
 * @param struct file *file -  pointer to the file structure of the caller
 * @return 0 - successfully, otherwise the error code (a negative number)
 *
 */
static int device_release(struct inode *inode, struct file *file)
{
	return SUCCESS;
}

/**
 * A function to handle the event when caller requests a device read operation
 *
 * @param struct file *file - pointer to the file structure of the caller
 * @param char __user *buffer - pointer to the buffer in the user space
 * @param size_t length - size in bytes for the read operation
 * @param loff_t * offset
 * @return greater than 0 - number of bytes actually read, otherwise the error code (a negative number)
 *
 */
static ssize_t device_read(struct file *file, char __user *buffer, size_t length, loff_t * offset)
{
	ssize_t retval = SUCCESS;
	size_t act;
	size_t total;

	if(mutex_lock_killable(&dataOpLock) != SUCCESS) {
		printk(KERN_ALERT "Could not lock the mutex\n");
		return -EPERM;
	}


	if (!isEntropySrcRdy || isShutDown) {
		retval = -ENODATA;
	} else {
		isDeviceOpPending = true;
		total = 0;
		do {
			retval = get_entropy_bytes();
			if (retval == SUCCESS) {
				act = TRND_OUT_BUFFSIZE - curTrngOutIdx;
				if (act > (length - total)) {
					act = (length - total);
				}
				if (copy_to_user(buffer + total, buffTRndOut + curTrngOutIdx, act)) {
					retval = -EFAULT;
					break;
				} else {
					curTrngOutIdx += act;
					total += act;
					retval = total;
				}
			} else {
				break;
			}
		} while (total < length);
	#ifdef inDebugMode
		if (total > length) {
			printk(KERN_ALERT "Expected %d bytes to read and actually got %d \n", (int)length, (int)total);
		}
	#endif

	}
	isDeviceOpPending = false;
	mutex_unlock(&dataOpLock);
	return retval;
}

/**
 * A function to request new entropy bytes when running out of entropy in the local buffer
 *
 * @return 0 - successful operation, otherwise the error code (a negative number)
 *
 */
static int get_entropy_bytes(void) {
	int status;
	if(curTrngOutIdx >= TRND_OUT_BUFFSIZE) {
		status = rcv_rnd_bytes();
	} else {
		status = SUCCESS;
	}
	return status;
}

/**
 * A function to fill the buffer with new entropy bytes
 *
 * @return 0 - successful operation, otherwise the error code (a negative number)
 *
 */
static int rcv_rnd_bytes(void) {
	int retval;
   	uint8_t lowByteCount;
   	uint8_t highByteCount;
   	uint16_t byteCnt;
   	int i,j;
   	uint32_t *dst;

	if (!isEntropySrcRdy || isShutDown) {
		return -EPERM;
	}

	isUsbOpPending = true;

	byteCnt = RND_IN_BUFFSIZE;
   	lowByteCount  = byteCnt & 0x00ff;
   	highByteCount = byteCnt >> 8;

   	usbData->bulk_out_buffer[0] = 'x';
	usbData->bulk_out_buffer[1] = lowByteCount;
	usbData->bulk_out_buffer[2] = highByteCount;

	retval = snd_rcv_usb_data(usbData->bulk_out_buffer, 3, buffRndIn, RND_IN_BUFFSIZE, USB_READ_TIMEOUT_SECS);
	if (retval == SUCCESS) {
		dst = (uint32_t *)buffTRndOut;
		rct_restart();
		apt_restart();
		for (i = 0; i < RND_IN_BUFFSIZE / WORD_SIZE_BYTES; i += MIN_INPUT_NUM_WORDS) {
			for (j = 0; j < MIN_INPUT_NUM_WORDS; j++) {
				srcToHash[j] = ((uint32_t *)buffRndIn)[i+j];
			}
			sha256_stampSerialNumber(srcToHash);
			sha256_generateHash(srcToHash, MIN_INPUT_NUM_WORDS + 1, dst);
			dst += OUT_NUM_WORDS;
		}
		curTrngOutIdx = 0;
		for (i = 0; i < TRND_OUT_BUFFSIZE; i++) {
			rct_sample(buffTRndOut[i]);
			apt_sample(buffTRndOut[i]);
		}

		if (rct.statusByte != SUCCESS) {
			printk(KERN_ALERT "Repetition Count Test failure\n");
			retval = -EPERM;
		} else if (apt.statusByte != SUCCESS) {
			printk(KERN_ALERT "Adaptive Proportion Test failure\n");
			retval = -EPERM;
		}
	}

	isUsbOpPending = false;
	return retval;
}

/**
 * Send a TL device command and receive response
 *
 * @param char *snd -  a pointer to the command
 * @param int sizeSnd - how many bytes in command
 * @param char *rcv - a pointer to the data receive buffer
 * @param int sizeRcv - how many bytes expected to receive
 * @param int opTimeoutSecs - device read time out value in seconds
 *
 * @return 0 - successful operation, otherwise the error code (a negative number)
 *
 */
static int snd_rcv_usb_data(char *snd, int sizeSnd, char *rcv, int sizeRcv, int opTimeoutSecs) {
	int retry;
	int actualcCnt;
	int retval = SUCCESS;

	for (retry = 0; retry < USB_READ_MAX_RETRY_CNT; retry++) {
		if (isShutDown) {
			return -EPERM;
		}
		retval = usb_bulk_msg(usbData->udev, usb_sndbulkpipe(usbData->udev, usbData->bulk_out_endpointAddr), snd, sizeSnd, &actualcCnt, HZ*10);
		if (retval == SUCCESS && actualcCnt == sizeSnd) {
			retval = chip_read_data(rcv, sizeRcv + 1, opTimeoutSecs);
			if (retval == SUCCESS) {
				if (rcv[sizeRcv] != 0) {
					retval = -EFAULT;
					#ifdef inDebugMode
						printk(KERN_INFO "Received an invalid device status code %d\n", rcv[sizeRcv]);
					#endif
				} else {
					break;
				}
			}
		} else {
			continue;
		}
	}
	if (retry >= USB_READ_MAX_RETRY_CNT) {
		retval = -ETIMEDOUT;
	}
	return retval;
}

/**
 * A function to handle TL device receive command
 * @param char *buff - a pointer to the data receive buffer
 * @param int length - how many bytes expected to receive
 * @param int opTimeoutSecs - device read time out value in seconds

 * @return 0 - successful operation, otherwise the error code (a negative number)
 *
 */
static int chip_read_data(char *buff, int length, int opTimeoutSecs) {
	long secsWaited;
	int transferred;
	ktime_t start, end;
	int cnt;
	int i;
	int retval;

	start = get_seconds();

	cnt = 0;
	do {
		if (isShutDown) {
			return -EPERM;
		}
		retval = usb_bulk_msg(usbData->udev, usb_rcvbulkpipe(usbData->udev, usbData->bulk_in_endpointAddr), usbData->bulk_in_buffer, USB_BUFFER_SIZE, &transferred, HZ*50);
		#ifdef inDebugMode
			printk(KERN_INFO "chip_read_data retval %d transferred %d, length %d\n", retval, transferred, length);
		#endif
		if (retval) {
			return retval;
		}

		if (transferred > USB_BUFFER_SIZE) {
			printk(KERN_ALERT "Received unexpected bytes when processing USB device request\n");
			return -EFAULT;
		}

		end = get_seconds();
		secsWaited = end - start;
		if (transferred > 2) {
			for (i = 0; i < transferred; i++) {
				if ( (i % usbData->bulk_in_size) == 0) {
					i++;
					continue;
				} else {
					buff[cnt++] = usbData->bulk_in_buffer[i];
					if (cnt >= length) {
						break;
					}
				}
			}
		}
	} while ( cnt < length && secsWaited < opTimeoutSecs);

	if (cnt != length) {
		#ifdef inDebugMode
			printk(KERN_INFO "timeout received, cnt %d\n", cnt);
		#endif
		return -ETIMEDOUT;
	}

	return SUCCESS;
}

/**
 * A function to handle the event when caller requests a device write operation
 *
 * @param struct file *file - pointer to the file structure of the caller
 * @param char __user *buff - pointer to the buffer in the user space
 * @param size_t len - size in bytes for the read operation
 * @param loff_t * off
 * @return greater than 0 - number of bytes actually written, otherwise the error code (a negative number)
 *
 */
static ssize_t device_write(struct file *file, const char *buff, size_t len, loff_t * off)
{
	return -EPERM;
}

/**
 * A function to handle the event when caller requests file seek operation
 *
 * @param struct file *file - pointer to the file structure of the caller
 * @param off_t off - seek position
 * @param int whence - seek mode
 * @return greater than 0 - new offset position, otherwise the error code (a negative number)
 *
 */
static loff_t device_llseek(struct file *filp, loff_t off, int whence)
{
	return -EPERM;
}

/*
 * A function for handling module loading event
 * @return greater than 0 - number of bytes actually read, otherwise the error code (a negative number)
 */
static int __init init_tlrandom(void)
{
	int usb_result;
	int err;

	err = 0;
	usbData = NULL;
	buffRndIn = NULL;
	buffTRndOut = NULL;

	mutex_init(&dataOpLock);

	rct_initialize();
	apt_initialize();

	sha256_initializeSerialNumber(413145);
	if (sha256_selfTest() != SUCCESS) {
		printk(KERN_ALERT "Post processing logic failed the self-test\n");
		return -EPERM;
	}

	err = init_char_dev();
	if (err != SUCCESS) {
		printk(KERN_ALERT "Could not initialize characetr device %s\n", DEVICE_NAME);
		return err;
	}

//	major = register_chrdev(0, DEVICE_NAME, &fops);
//
//	if (major < 0) {
//		printk(KERN_ALERT "Could not register the char device %s, error code: %d\n", DEVICE_NAME, major);
//		return major;
//	}

	// Initialize buffers
	buffRndIn = kmalloc(RND_IN_BUFFSIZE + 1, GFP_KERNEL);
	if (buffRndIn == NULL) {
		printk(KERN_ALERT "Could not allocate %d kernel bytes for the random input buffer\n", RND_IN_BUFFSIZE);
		//unregister_chrdev(major, DEVICE_NAME);
		uninit_char_dev();
		return -ENOMEM;
	}

	buffTRndOut = kmalloc(TRND_OUT_BUFFSIZE, GFP_KERNEL);
	if (buffTRndOut == NULL) {
		printk(KERN_ALERT "Could not allocate %d kernel bytes for the random output buffer\n", TRND_OUT_BUFFSIZE);
		//unregister_chrdev(major, DEVICE_NAME);
		uninit_char_dev();
		kfree(buffRndIn);
		return -ENOMEM;
	}

	usb_result = usb_register(&usb_driver);
	if (usb_result < 0) {
		printk(KERN_ALERT "Could not register usb driver, error number %d\n", usb_result);
		//unregister_chrdev(major, DEVICE_NAME);
		uninit_char_dev();
		kfree(buffRndIn);
		kfree(buffTRndOut);
		return usb_result;
	}

	printk(KERN_INFO "Char device %s registered successfully with the major number %d, module version: %s\n", DEVICE_NAME, major, DEVICE_VERSION);
	return SUCCESS;
}

/**
 * Initialize the character device
 *
 * $return int - SUCCESS or error number
 *
 */
static int init_char_dev(void) {
	int error;
	int devices_to_destroy;
	dev_t dev;

	error = SUCCESS;
	dev = 0;
	devices_to_destroy = 0;

	error = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
	if (error < 0) {
		printk(KERN_ALERT "alloc_chrdev_region() call failed with error: %d\n", error);
		return error;
	}
	major = MAJOR(dev);

	dev_class = class_create(THIS_MODULE, DEVICE_NAME);
	if (IS_ERR(dev_class)) {
		error = PTR_ERR(dev_class);
		goto fail;
	}

	cdv = (struct cdev *)kzalloc(sizeof(struct cdev), GFP_KERNEL);
	if (cdv == NULL) {
		error = -ENOMEM;
		goto fail;
	}

	error = create_device();
	if (error) {
		goto fail;
	}

	return error;

fail:
	uninit_char_dev();
	return error;

}

/**
 * Create the device
 *
 * $return int - SUCCESS or error number
 */
static int create_device(void) {
	int error;
	dev_t devno;
	struct device *device;

	error = SUCCESS;
	device = NULL;
	devno = MKDEV(major, minor);
	cdev_init(cdv, &fops);
	cdv->owner = THIS_MODULE;
	error = cdev_add(cdv, devno, 1);
	if (error)
	{
		printk(KERN_ALERT "cdev_add() call failed with error: %d\n", error);
		return error;
	}
	device = device_create(dev_class, NULL, devno, NULL, DEVICE_NAME);
	if (IS_ERR(device)) {
		error = PTR_ERR(device);
		printk(KERN_ALERT "device_create() failed with error: %d\n", error);
		return error;
	}

	return error;
}

/**
 * Un-initialize the character device
 *
 */
static void uninit_char_dev(void) {
	// Get rid of the device
	if (cdv) {
		device_destroy(dev_class, MKDEV(major, minor));
		cdev_del(cdv);
		kfree(cdv);
	}
	if (dev_class) {
		class_destroy(dev_class);
	}
	unregister_chrdev_region(MKDEV(major, 0), 1);
}

/*
 * A function to handle event for module unloading
 */
static void __exit exit_tlrandom(void)
{
	isEntropySrcRdy = false;
	isShutDown = true;
	msleep(2000);
	wait_for_pending_ops();
	usb_deregister(&usb_driver);
	//unregister_chrdev(major, DEVICE_NAME);
	uninit_char_dev();
	kfree(buffRndIn);
	kfree(buffTRndOut);
	mutex_destroy(&dataOpLock);
	printk(KERN_INFO "Char device %s unregistered successfully\n", DEVICE_NAME);
}

/**
 * A function to wait a little for any pending operations used when unloading the module
 *
 */
static void wait_for_pending_ops(void) {
	int cnt;
	for(cnt = 0; cnt < 100 && (isDeviceOpPending == true || isUsbOpPending == true); cnt++) {
		msleep(100);
	}
}

/**
 * Initialize the SHA256 data
 *
 */
static void sha256_initialize(void) {
	// Initialize H0, H1, H2, H3, H4, H5, H6 and H7
	sd.h0 = 0x6a09e667;
	sd.h1 = 0xbb67ae85;
	sd.h2 = 0x3c6ef372;
	sd.h3 = 0xa54ff53a;
	sd.h4 = 0x510e527f;
	sd.h5 = 0x9b05688c;
	sd.h6 = 0x1f83d9ab;
	sd.h7 = 0x5be0cd19;
}

/**
 * Stamp a new serial number for the input data block into the last word
 *
 * @param void* inputBlock pointer to the input hashing block
 *
 */
static void sha256_stampSerialNumber(void *inputBlock)
{
	uint32_t *inw = (uint32_t*)inputBlock;
	inw[MIN_INPUT_NUM_WORDS] = sd.blockSerialNumber++;
}

/**
 * Initialize the serial number for hashing
 *
 * @param uint32_t initValue - a startup random number for generating serial number for hashing
 *
 */
static void sha256_initializeSerialNumber(uint32_t initValue) {
	sd.blockSerialNumber = initValue;
}

/**
 * Generate SHA256 value.
 *
 * @param uint32_t* src - pointer to an array of 32 bit words used as hash input
 * @param uint32_t dst - pointer to an array of 8 X 32 bit words used as hash output
 * @param int16_t len - number of 32 bit words available in array pointed by 'src'
 *
 * @return int 0 for successful operation, -1 for invalid parameters
 *
 */
static int sha256_generateHash(uint32_t *src, int16_t len, uint32_t *dst) {

	uint16_t blockNum;
	uint8_t ui8;
	int32_t initialMessageSize;
	uint16_t numCompleteDataBlocks;
	uint16_t reminder;
	uint16_t srcOffset;
	uint8_t needAdditionalBlock;
	uint8_t needToAddOneMarker;

	if (len <= 0) {
		return -1;
	}

	sha256_initialize();

	initialMessageSize = len * 8 * 4;
	numCompleteDataBlocks = len / maxDataBlockSizeWords;
	reminder = len % maxDataBlockSizeWords;

	// Process complete blocks
	for (blockNum = 0; blockNum < numCompleteDataBlocks; blockNum++) {
		srcOffset = blockNum * maxDataBlockSizeWords;
		for (ui8 = 0; ui8 < maxDataBlockSizeWords; ui8++) {
			sd.w[ui8] = src[ui8 + srcOffset];
		}
		// Hash the current block
		sha256_hashCurrentBlock();
	}

	srcOffset = numCompleteDataBlocks * maxDataBlockSizeWords;
	needAdditionalBlock = 1;
	needToAddOneMarker = 1;
	if (reminder > 0) {
		// Process the last data block if any
		ui8 = 0;
		for (; ui8 < reminder; ui8++) {
			sd.w[ui8] = src[ui8 + srcOffset];
		}
		// Append '1' to the message
		sd.w[ui8++] = 0x80000000;
		needToAddOneMarker = 0;
		if (ui8 < maxDataBlockSizeWords - 1) {
			for (; ui8 <  maxDataBlockSizeWords - 2; ui8++) {
				// Fill with zeros
				sd.w[ui8] = 0x0;
			}
			// add the message size to the current block
			sd.w[ui8++] = 0x0;
			sd.w[ui8] = initialMessageSize;
			sha256_hashCurrentBlock();
			needAdditionalBlock = 0;
		} else {
			// Fill the rest with '0'
			// Will need to create another block
			sd.w[ui8] = 0x0;
			sha256_hashCurrentBlock();
		}
	}

	if (needAdditionalBlock) {
		ui8 = 0;
		if (needToAddOneMarker) {
			sd.w[ui8++] = 0x80000000;
		}
		for (; ui8 <  maxDataBlockSizeWords - 2; ui8++) {
			sd.w[ui8] = 0x0;
		}
		sd.w[ui8++] = 0x0;
		sd.w[ui8] = initialMessageSize;
		sha256_hashCurrentBlock();
	}

	// Save the results
	dst[0] = sd.h0;
	dst[1] = sd.h1;
	dst[2] = sd.h2;
	dst[3] = sd.h3;
	dst[4] = sd.h4;
	dst[5] = sd.h5;
	dst[6] = sd.h6;
	dst[7] = sd.h7;

	return 0;
}

/**
 * Hash current block
 *
 */
static void sha256_hashCurrentBlock(void) {
	uint8_t t;

	// Process elements 16...63
	for (t = 16; t <= 63; t++) {
		sd.w[t] = sha256_sigma1(&sd.w[t-2]) + sd.w[t-7] + sha256_sigma0(&sd.w[t-15]) + sd.w[t-16];
	}

	// Initialize variables
	sd.a = sd.h0;
	sd.b = sd.h1;
	sd.c = sd.h2;
	sd.d = sd.h3;
	sd.e = sd.h4;
	sd.f = sd.h5;
	sd.g = sd.h6;
	sd.h = sd.h7;

	// Process elements 0...63
	for (t = 0; t <= 63; t++) {
		sd.tmp1 = sd.h + sha256_sum1(&sd.e) + sha256_ch(&sd.e, &sd.f, &sd.g) + k[t] + sd.w[t];
		sd.tmp2 = sha256_sum0(&sd.a) + sha256_maj(&sd.a, &sd.b, &sd.c);
		sd.h = sd.g;
		sd.g = sd.f;
		sd.f = sd.e;
		sd.e = sd.d + sd.tmp1;
		sd.d = sd.c;
		sd.c = sd.b;
		sd.b = sd.a;
		sd.a = sd.tmp1 + sd.tmp2;
	}

	// Calculate the final hash for the block
	sd.h0 += sd.a;
	sd.h1 += sd.b;
	sd.h2 += sd.c;
	sd.h3 += sd.d;
	sd.h4 += sd.e;
	sd.h5 += sd.f;
	sd.h6 += sd.g;
	sd.h7 += sd.h;
}

/**
 * FIPS PUB 180-4 section 4.1.2 formula (4.2)
 *
 * @param uint32_t* x pointer to variable x
 * @param uint32_t* y pointer to variable y
 * @param uint32_t* z pointer to variable z
 * $return uint32_t Ch value
 *
 */
static uint32_t sha256_ch(uint32_t *x, uint32_t *y, uint32_t *z) {
	return  ((*x) & (*y)) ^ (~(*x) & (*z));
}

/**
 * FIPS PUB 180-4 section 4.1.2 formula (4.3)
 *
 * @param uint32_t* x pointer to variable x
 * @param uint32_t* y pointer to variable y
 * @param uint32_t* z pointer to variable z
 * $return uint32_t Maj value
 *
 */
static uint32_t sha256_maj(uint32_t *x, uint32_t *y, uint32_t *z) {
	return ((*x) & (*y)) ^ ((*x) & (*z)) ^ ((*y) & (*z));
}

/**
 * FIPS PUB 180-4 section 4.1.2 formula (4.4)
 *
 * @param uint32_t* x pointer to variable x
 * $return uint32_t Sum0 value
 *
 */
static uint32_t sha256_sum0(uint32_t *x) {
	return ROTR(2, *x) ^ ROTR(13, *x) ^ ROTR(22, *x);
}

/**
 * FIPS PUB 180-4 section 4.1.2 formula (4.5)
 *
 * @param uint32_t* x pointer to variable x
 * $return uint32_t Sum1 value
 *
 */
static uint32_t sha256_sum1(uint32_t *x) {
	return ROTR(6, *x) ^ ROTR(11, *x) ^ ROTR(25, *x);
}

/**
 * FIPS PUB 180-4 section 4.1.2 formula (4.6)
 *
 * @param uint32_t* x pointer to variable x
 * $return uint32_t sigma0 value
 *
 */
static uint32_t sha256_sigma0(uint32_t *x) {
	return ROTR(7, *x) ^ ROTR(18, *x) ^ ((*x) >> 3);
}

/**
 * FIPS PUB 180-4 section 4.1.2 formula (4.7)
 *
 * @param uint32_t* x pointer to variable x
 * $return uint32_t sigma1 value
 *
 */
static uint32_t sha256_sigma1(uint32_t *x) {
	return ROTR(17, *x) ^ ROTR(19, *x) ^ ((*x) >> 10);
}

/**
 * Run the self test for checking the SHA algorithm implementation
 *
 * @return int 0 for successful operation
 *
 */
static int sha256_selfTest(void) {
	uint32_t results[8];
	int retVal;

	retVal = sha256_generateHash((uint32_t*)testSeq1, (uint16_t)11, (uint32_t*)results);
	if (retVal == 0) {
		// Compare the expected with actual results
		retVal = memcmp(results, exptHashSeq1, 8);
	}
	return retVal;
}

static void rct_initialize(void) {
	memset(&rct, 0x00, sizeof (rct));
	rct.statusByte = 0;
	rct.signature = 1;
	rct.maxRepetitions = 5;
	rct_restart();
}

static void rct_restart(void) {
	rct.isInitialized = false;
	rct.curRepetitions = 1;
	rct.failureWindow = 0;
	rct.failureCount = 0;
}

static void rct_sample(uint8_t value) {
	if (!rct.isInitialized) {
		rct.isInitialized = true;
		rct.lastSample = value;
	} else {
		if (rct.lastSample == value) {
			rct.curRepetitions++;
			if (rct.curRepetitions >= rct.maxRepetitions) {
				rct.curRepetitions = 1;
				if (++rct.failureCount >= numConsecFailThreshold) {
					if (rct.statusByte == 0) {
						rct.statusByte = rct.signature;
					}
				}
			}

		} else {
			rct.lastSample = value;
			rct.curRepetitions = 1;
		}
	}

}


static void apt_initialize(void) {
	memset(&apt, 0x00, sizeof (apt));
	apt.statusByte = 0;
	apt.signature = 2;
	apt.windowSize = 64;
	apt.cutoffValue = 5;
	apt_restart();
}

static void apt_restart(void) {
	apt.isInitialized = false;
	apt_restart_cycle();
}

static void apt_restart_cycle(void) {
	apt.cycleFailures = 0;
}

static void apt_sample(uint8_t value) {
	if (!apt.isInitialized) {
		apt.isInitialized = true;
		apt.firstSample = value;
		apt.curRepetitions = 0;
		apt.curSamples = 0;
	} else {
		if (++apt.curSamples >= apt.windowSize) {
			apt.isInitialized = false;
		}
		if (apt.firstSample == value) {
			if (++apt.curRepetitions > apt.cutoffValue) {
				// Check to see if we have reached the failure threshold
				if (++apt.cycleFailures >= numConsecFailThreshold) {
					if (apt.statusByte == 0) {
						apt.statusByte = apt.signature;
					}
				}
			} else {
				apt_restart_cycle();
			}
		}
	}
}

module_init( init_tlrandom);
module_exit( exit_tlrandom);