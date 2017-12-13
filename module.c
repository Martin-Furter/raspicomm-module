#include <linux/module.h>     // Needed by all modules
#include <linux/kernel.h>     // Needed for KERN_INFO
#include <linux/init.h>       // Needed for the macros
#include <linux/fs.h>         // Needed for the file structure & register_chrdev()
#include <linux/tty.h>        // 
#include <linux/tty_driver.h> // Needed for struct tty_driver
#include <linux/gpio.h>       // needed for gpio_X() calls
#include <linux/interrupt.h>  // Needed for request_interrupt()
#include <linux/workqueue.h>
#include <linux/delay.h>      // Needed for udelay
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/version.h>    /* needed for KERNEL_VERSION() macro */
#if 0 // +++--
#include <asm/io.h>           // Needed for ioremap & iounmap
#include <asm/uaccess.h>
#endif
#include <linux/spi/spi.h>
#if 0 // +++--
#include "platform.h"
#endif
#include "module.h"
#include "queue.h"     // needed for queue_xxx functions

// MajorDriverNumber == 0 is using dynamically number
static const int RaspicommMajorDriverNumber = 0;

static DEFINE_SPINLOCK(dev_lock);

#if 0 // +++--
// struct that holds the gpio configuration
typedef struct {
  int gpio;             // set to the gpio that should be requested
  int gpio_alternative; // set to the alternative that the gpio should be configured
  int gpio_requested;   // set if the gpio was successfully requested, otherwise 0
} gpioconfig;
#endif

typedef enum {
  STOPBITS_ONE = 0,
  STOPBITS_TWO = 1
} Stopbits;

typedef enum {
  DATABITS_7 = 1,
  DATABITS_8 = 0
} Databits;

typedef enum {
  PARITY_OFF = 0,
  PARITY_ON =  1
} Parity;

typedef enum {
  MAX3140_WRITE_DATA_R = 1 << 15,
  MAX3140_WRITE_DATA_TE = 1 << 10,
  MAX3140_WRITE_DATA_RTS = 1 << 9
} MAX3140_WRITE_DATA_t;

typedef enum {
  MAX3140_UART_R     = 1 << 15, 
  MAX3140_UART_T     = 1 << 14,
  MAX3140_UART_FEN   = 1 << 13,
  MAX3140_UART_SHDNo = 1 << 12,
  MAX3140_UART_TM    = 1 << 11,
  MAX3140_UART_RM    = 1 << 10,
  MAX3140_UART_PM    = 1 << 9,
  MAX3140_UART_RAM   = 1 << 8,
  MAX3140_UART_IR    = 1 << 7,
  MAX3140_UART_ST    = 1 << 6,
  MAX3140_UART_PE    = 1 << 5,  // Parity Enable
  MAX3140_UART_L     = 1 << 4,
  MAX3140_UART_B3    = 1 << 3,
  MAX3140_UART_B2    = 1 << 2,
  MAX3140_UART_B1    = 1 << 1,
  MAX3140_UART_B0    = 1 << 0,

  MAX3140_wd_Pt = 1 << 8  

} MAX3140_UartFlags;

#define MAX3140_WRITE_CONFIG ( MAX3140_UART_R | MAX3140_UART_T )
#define MAX3140_READ_CONFIG ( MAX3140_UART_T )
#define MAX3140_READ_DATA ( 0 )
#define MAX3140_WRITE_DATA ( MAX3140_UART_R )

// funny that this function is not standard
static inline int
spi_transceive(struct spi_device *spi, void *tx_buf, void *rx_buf, size_t len)
{
	struct spi_transfer	t = {
			.tx_buf		= tx_buf,
			.rx_buf		= rx_buf,
			.len		= len,
		};

	return spi_sync_transfer(spi, &t, 1);
}

// ****************************************************************************
// **** START raspicomm private functions ****
// ****************************************************************************
// forward declarations of private functions
static int __init raspicomm_init(void);
static void __exit raspicomm_exit(void);

static int           raspicomm_max3140_get_swbacksleep   (speed_t speed);
static unsigned char raspicomm_max3140_get_baudrate_index(speed_t speed);
static unsigned int  raspicomm_max3140_get_uart_config   (speed_t speed, 
                                                          Databits databits, 
                                                          Stopbits stopbits, 
                                                          Parity parity);
static void          raspicomm_max3140_configure         (speed_t speed, 
                                                          Databits databits, 
                                                          Stopbits stopbits, 
                                                          Parity parity);
static bool          raspicomm_max3140_apply_config(void);

static int           raspicomm_spi0_send(unsigned int mosi);

static void          raspicomm_rs485_received(struct tty_struct* tty, char c);

irqreturn_t          raspicomm_irq_handler(int irq, void* dev_id);

#if 0 // +++--
static bool                   raspicomm_spi0_init(void);
volatile static unsigned int* raspicomm_spi0_init_mem(void);
static int                    raspicomm_spi0_init_gpio(void);
static void                   raspicomm_spi0_init_gpio_alt(int gpio, int alt);
static void                   raspicomm_spi0_deinit_gpio(void);
#endif
static int                    raspicomm_spi0_init_irq(void);
static void                   raspicomm_spi0_deinit_irq(void);
#if 0 // +++--
static void                   raspicomm_spi0_init_port(void);
static void                   raspicomm_spi0_deinit_mem(volatile unsigned int* spi0);
#endif

// ****************************************************************************
// *** END raspicomm private functions ****
// ****************************************************************************

// ****************************************************************************
// **** START raspicommDriver functions ****
// ****************************************************************************
static int  raspicommDriver_open (struct tty_struct *, struct file *);
static void raspicommDriver_close(struct tty_struct *, struct file *);
static int  raspicommDriver_write(struct tty_struct *, 
                                  const unsigned char *, 
                                  int);
static int  raspicommDriver_write_room(struct tty_struct *);
static void raspicommDriver_flush_buffer(struct tty_struct *);
static int  raspicommDriver_chars_in_buffer(struct tty_struct *);
static void raspicommDriver_set_termios(struct tty_struct *, struct ktermios *);
static void raspicommDriver_stop(struct tty_struct *);
static void raspicommDriver_start(struct tty_struct *);
static void raspicommDriver_hangup(struct tty_struct *);
static int  raspicommDriver_tiocmget(struct tty_struct *tty);
static int  raspicommDriver_tiocmset(struct tty_struct *tty,
                                     unsigned int set, 
                                     unsigned int clear);
static int  raspicommDriver_ioctl(struct tty_struct* tty,
                                  unsigned int cmd,
                                  unsigned int long arg);
static void raspicommDriver_throttle(struct tty_struct * tty);
static void raspicommDriver_unthrottle(struct tty_struct * tty);
static void raspicomm_start_transfer(void);

// ****************************************************************************
// **** END raspicommDriver functions ****
// ****************************************************************************

// ****************************************************************************
// **** START raspicomm private fields ****
// ****************************************************************************
static const struct tty_operations raspicomm_ops = {
  .open            = raspicommDriver_open,
  .close           = raspicommDriver_close,
  .write           = raspicommDriver_write,
  .write_room      = raspicommDriver_write_room,
  .flush_buffer    =raspicommDriver_flush_buffer,
  .chars_in_buffer = raspicommDriver_chars_in_buffer,
  .ioctl           = raspicommDriver_ioctl,
  .set_termios     = raspicommDriver_set_termios,
  .stop            = raspicommDriver_stop,
  .start           = raspicommDriver_start,
  .hangup          = raspicommDriver_hangup,
  .tiocmget        = raspicommDriver_tiocmget,
  .tiocmset        = raspicommDriver_tiocmset,
  .throttle        = raspicommDriver_throttle,
  .unthrottle      = raspicommDriver_unthrottle
};

#define IRQ_DEV_NAME "raspicomm"
#define PORT_COUNT 1

// the driver instance
static struct tty_driver* raspicommDriver;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
static struct tty_port Port;
#endif

// the number of open() calls
static int OpenCount = 0;

// ParityIsEven == true ? even : odd
static int ParityIsEven = 1;
static int ParityEnabled = 0;

// currently opened tty device
static struct tty_struct* OpenTTY = NULL;

// transmit queue
static queue_t TxQueue;

// variable used in the delay to simulate the baudrate
static int SwBacksleep;

// config setting of the spi0
static int SpiConfig;

static struct spi_device *spi_slave;
#if 0 // +++--
// Spi0 memory interface pointer
volatile static unsigned int* Spi0;
#endif

// The requested gpio (set by raspicomm_spi0_init_gpio and freed by raspicomm_spi0_deinit_gpio)
static int Gpio;

// The interrupt that signals when data is available
static int Gpio17_Irq;

#if 0 // +++--
// the configured gpios
static gpioconfig GpioConfigs[] =  { {7}, {8}, {9}, {10}, {11} };
#endif

// ****************************************************************************
// **** END raspicomm private fields
// ****************************************************************************


// ****************************************************************************
// **** START module specific functions ****
// ****************************************************************************
// module entry point function - gets called from insmod
module_init(raspicomm_init);

// module exit point function - gets called from rmmod
module_exit(raspicomm_exit);
// ****************************************************************************
// **** END module specific functions ****
// ****************************************************************************

// ****************************************************************************
// **** START raspicomm private function implementations
// ****************************************************************************
// initialization function that gets called when the module is loaded
static int __init raspicomm_init()
{
  struct spi_master *master;
  struct spi_board_info spi_device_info = {
        .modalias = "raspicommrs485",
        .max_speed_hz = 1000000, //speed of your device splace can handle
        .bus_num = 0, //BUS number
        .chip_select = 0,
        .mode = SPI_MODE_0,
  };
#if 0 // +++--
  Gpio = Gpio17_Irq = -EINVAL;;
#endif
  SpiConfig = 0;

  // log the start of the initialization
  LOG("kernel module initialization");

  master = spi_busnum_to_master(0);
  if(!master)
  {
    return -ENODEV;
  }
  spi_slave = spi_new_device(master, &spi_device_info);
  if(!spi_slave)
  {
    return -ENODEV;
  }
  if( !raspicomm_spi0_init_irq() )
  {
    return -ENODEV;
  }
#if 0 // +++--
  // initialize the spi0
  if (!raspicomm_spi0_init()) {
    return -ENODEV;
  }
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)

  /* initialize the port */
  tty_port_init(&Port);
  Port.low_latency = 1;

  /* allocate the driver */
  raspicommDriver = tty_alloc_driver(PORT_COUNT, TTY_DRIVER_REAL_RAW);

  /* return if allocation fails */
  if (IS_ERR(raspicommDriver))
    return -ENOMEM;
#else
  /* allocate the driver */
  raspicommDriver = alloc_tty_driver(PORT_COUNT);

  /* return if allocation fails */
  if (!raspicommDriver)
    return -ENOMEM;
#endif

  // init the driver
  raspicommDriver->owner                 = THIS_MODULE;
  raspicommDriver->driver_name           = "raspicomm rs485";
  raspicommDriver->name                  = "ttyRPC";
  raspicommDriver->major                 = RaspicommMajorDriverNumber;
  raspicommDriver->minor_start           = 0;
  //raspicommDriver->flags                 = TTY_DRIVER_REAL_RAW;
  raspicommDriver->type                  = TTY_DRIVER_TYPE_SERIAL;
  raspicommDriver->subtype               = SERIAL_TYPE_NORMAL;
  raspicommDriver->init_termios          = tty_std_termios;
  raspicommDriver->init_termios.c_ispeed = 9600;
  raspicommDriver->init_termios.c_ospeed = 9600;
  raspicommDriver->init_termios.c_cflag  = B9600 | CREAD | CS8 | CLOCAL;

  // initialize function callbacks of tty_driver, necessary before tty_register_driver()
  tty_set_operations(raspicommDriver, &raspicomm_ops);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
  /* link the port with the driver */
  tty_port_link_device(&Port, raspicommDriver, 0);
#endif
  
  // try to register the tty driver
  if (tty_register_driver(raspicommDriver))
  {
    LOG("tty_register_driver failed");
    put_tty_driver(raspicommDriver);
    return -1; // return if registration fails
  }

  LOG ("raspicomm_init() completed");

  /* successfully initialized the module */
  return 0; 
}

// cleanup function that gets called when the module is unloaded
static void __exit raspicomm_exit()
{
  LOG ("raspicomm_exit() called");

  // unregister the driver
  if (tty_unregister_driver(raspicommDriver))
    LOG("tty_unregister_driver failed");

  put_tty_driver(raspicommDriver);

#if 0 // +++--
  // free mapped memory
  raspicomm_spi0_deinit_mem(Spi0);
#endif

  // free the irq
  raspicomm_spi0_deinit_irq();

#if 0 // +++--
  // free gpio
  raspicomm_spi0_deinit_gpio();
#endif
  if( spi_slave )
  {
    spi_unregister_device(spi_slave);
  }

  // log the unloading of the rs-485 module
  LOG("kernel module exit");
}


// helper function
static unsigned char raspicomm_max3140_get_baudrate_index(speed_t speed)
{
  switch (speed)
  {
    case 600: return 0xF;
    case 1200: return 0xE;
    case 2400: return 0xD;
    case 4800: return 0xC;
    case 9600: return 0xB;
    case 19200: return 0xA;
    case 38400: return 0x9;
    case 57600: return 0x2;
    case 115200: return 0x1;
    case 230400: return 0x0;
    default: return raspicomm_max3140_get_baudrate_index(9600);
  }
}

// helper function that creates the config for spi
static unsigned int raspicomm_max3140_get_uart_config(speed_t speed, Databits databits, Stopbits stopbits, Parity parity)
{
  unsigned int value = 0;

  value |= MAX3140_WRITE_CONFIG;

  value |= MAX3140_UART_RM;

  value |= raspicomm_max3140_get_baudrate_index(speed);

  value |= stopbits << 6;

  value |= parity << 5;

  value |= databits << 4;

  return value;
}

static int raspicomm_max3140_get_swbacksleep(speed_t speed)
{
  return 10000000 / speed;
}

static void raspicomm_max3140_configure(speed_t speed, Databits databits, Stopbits stopbits, Parity parity)
{
  int swBacksleep = raspicomm_max3140_get_swbacksleep(speed), 
      config = raspicomm_max3140_get_uart_config(speed, databits, stopbits, parity);

  LOG( "raspicomm_max3140_configure() called speed=%i, databits=%i, stopbits=%i, parity=%i => config: %X, swBacksleep: %i", speed, databits, stopbits, parity, config, swBacksleep);

  SpiConfig = config;
  SwBacksleep = swBacksleep;
}

// initializes the spi0 for supplied configuration
static bool raspicomm_max3140_apply_config()
{ 
  int rxconfig;

  /* configure the SPI */
  raspicomm_spi0_send( SpiConfig );

  /* read back the config */
  rxconfig = raspicomm_spi0_send(MAX3140_READ_CONFIG);

  if ((rxconfig & 0xFFF) != (SpiConfig & 0xFFF)) {
    return 0;
  }

  /* write data (R set, T not set) and enable receive by disabling RTS (TE set so that no data is sent) */
  raspicomm_spi0_send( MAX3140_WRITE_DATA_R | MAX3140_WRITE_DATA_TE | MAX3140_WRITE_DATA_RTS);

  return 1;
}

// Uncommented by javicient

static int raspicomm_max3140_get_parity_flag(char c)
{
  // even parity: is 1 if number of ones is odd -> making number of bits of value and parity = even
  // odd parity: is 1 if the number of ones is even -> making number of bits of value and parity = odd

  int parityEven = ParityIsEven;
  int parityEnabled = ParityEnabled;
  int count = 0, i;
  int ret;

  if (parityEnabled == 0)
    return 0;

 // count the number of ones  
   for (i = 0; i < 8; i++)
     if (c & (1 << i))
       count++;

   if (parityEven)
     ret = (count % 2) ? MAX3140_wd_Pt : 0;
   else
     ret = (count % 2) ? 0 : MAX3140_wd_Pt;

   LOG ( "raspicomm_max3140_get_parity_flag(c=%c) parityEven=%i, count=%i, ret=%i", c, parityEven, count, ret );

   return ret;
 }

static void raspicomm_start_transfer() {
  int rxdata, txdata;
  unsigned long flags; // AHB
  spin_lock_irqsave(&dev_lock, flags); // AHB vor Eintritt

  // TBE-interrupt enable, falls das noch nicht erledigt ist
  if (SpiConfig & MAX3140_UART_TM) { // bereits eingeschaltet --> nichts mehr tun, der TBE-IR sorgt schon irgendwann f�r ein Leeren der Queue
    rxdata = 0; //
  }
  else { // noch nicht eingeschaltet --> jetzt einschalten
    rxdata = raspicomm_spi0_send((SpiConfig = SpiConfig | MAX3140_WRITE_CONFIG | MAX3140_UART_TM));
  }

  if ((rxdata & MAX3140_UART_T)) { // TBE --> senden m�glich
    if (queue_dequeue(&TxQueue, &txdata)) { // Byte zum Senden da --> gleich erledigen
      /* send the data (RTS enabled) AHB rxdata f�r Log */
      rxdata = raspicomm_spi0_send(MAX3140_WRITE_DATA | txdata | raspicomm_max3140_get_parity_flag((char)txdata));
      LOG("raspicomm_start_transfer: 0x%X --> 0x%X", txdata, rxdata); // AHB
    } // Byte zum Senden da
  }

  spin_unlock_irqrestore(&dev_lock, flags); // AHB Freigabe des Locks
}

static int raspicomm_spi0_send(unsigned int mosi)
{
  unsigned char tx[2], rx[2];
  int rc;

  //LOG ("raspicomm_spi0_send(%X): %X spi0+1 %X spi0+2 %X", mosi, SPI0_CNTLSTAT, SPI0_FIFO, SPI0_CLKSPEED );
  tx[0] = mosi >> 8;
  tx[1] = mosi;
  rc = spi_transceive( spi_slave, tx, rx, 2 );
  if( rc < 0 )
  {
    return 0;
  }
  return (tx[0] << 8) | tx[1];
}

#if 0 // +++--
static int raspicomm_spi0_send(unsigned int mosi)
{
  // TODO direct pointer access should not be used -> use kernel functions iowriteX() instead see http://www.makelinux.net/ldd3/chp-9-sect-4
  unsigned char v1,v2;
  int status;

  //LOG ("raspicomm_spi0_send(%X): %X spi0+1 %X spi0+2 %X", mosi, SPI0_CNTLSTAT, SPI0_FIFO, SPI0_CLKSPEED );

  // Set up for single ended, MS comes out first
  v1 = mosi >> 8;
  v2 = mosi & 0x00FF;

  // Enable SPI interface: Use CS 0 and set activate bit
  SPI0_CNTLSTAT = SPI0_CS_CHIPSEL0 | SPI0_CS_ACTIVATE;

  // Write the command into the FIFO
  SPI0_FIFO = v1;
  SPI0_FIFO = v2;

  do {
     status = SPI0_CNTLSTAT;
  } while ( ((status & SPI0_CS_DONE) == 0) &&
            ((status & SPI0_TA) == SPI0_TA) );
  SPI0_CNTLSTAT = SPI0_CS_DONE; // clear the done bit

  if (((status & SPI0_CS_DONE) == 0) && ((status & SPI0_TA) == 0))
    LOG_INFO("spi transfer was not done, but transfer was not active anymore!");

  // Data from the ADC chip should now be in the receiver
  // read the received data
  v1 = SPI0_FIFO;
  v2 = SPI0_FIFO;

  LOG( "raspicomm_spi0_send(%X) recv: %X", mosi, ( (v1<<8) | (v2) ) );
  //if (use_backsleep)
  //  udelay(SwBacksleep);

  return ( (v1<<8) | (v2) );
}

// one time initialization for the spi0 
static bool raspicomm_spi0_init(void)
{
  bool success;
  // map the spi0 memory
  Spi0 = raspicomm_spi0_init_mem();

  // initialize the spi0
  raspicomm_spi0_init_port();

  // init the gpios
  raspicomm_spi0_init_gpio();

  // register the irq for the spi0
  raspicomm_spi0_init_irq();

  raspicomm_max3140_configure(9600, DATABITS_8, STOPBITS_ONE, PARITY_OFF);

  success = raspicomm_max3140_apply_config();

  if (!success)
  {
    // free mapped memory
    raspicomm_spi0_deinit_mem(Spi0);
    // free the irq
    raspicomm_spi0_deinit_irq();
    // free gpio
    raspicomm_spi0_deinit_gpio();
  }

  return success;
}

// map the physical memory that we need for spi0 access
volatile static unsigned int* raspicomm_spi0_init_mem(void)
{
  // in user space we would do mmap() call, in kernel space we do ioremap

  // call ioremap to map the physical address to something we can use
  unsigned int* p = ioremap(SPI0_BASE, 12);

  LOG( "ioremap(%X) returned %X", SPI0_BASE, (int)p);
  LOG( "spi0: %X spi0+1 %X spi0+2 %X", *p, *(p+1), *(p+2) );

  return p;
}
#endif

// irq handler, that gets fired when the gpio 17 falling edge occurs
irqreturn_t raspicomm_irq_handler(int irq, void* dev_id)
{
  int rxdata, txdata;

  // AHB Der Zugriff auf den UART wird durch einen Spinlock abgesichert (exklusiver Zugriff), somit kann auch
  //     ... start_transfer() auf die UART zugreifen
  unsigned long flags; // AHB
  spin_lock_irqsave(&dev_lock, flags); // AHB vor Eintritt

  // issue a read command to discover the cause of the interrupt
  rxdata = raspicomm_spi0_send(MAX3140_READ_DATA);

  /* if data is available in the receive register */
  if (rxdata & MAX3140_UART_R)
  {
    // handle the received data
    raspicomm_rs485_received(OpenTTY, rxdata & 0x00FF);
    LOG("raspicomm_irq recv: 0x%X", rxdata); // AHB
  }
  /* if the transmit buffer is empty */
  else if ((rxdata & MAX3140_UART_T))
  {
    /* get the data to send from the transmit queue */
    if (queue_dequeue(&TxQueue, &txdata))
    {
      /* send the data (RTS enabled) AHB rxdata f�r Log */
      rxdata = raspicomm_spi0_send(MAX3140_WRITE_DATA | txdata | raspicomm_max3140_get_parity_flag((char)txdata));
      LOG("raspicomm_irq sent: 0x%X --> 0x%X", txdata, rxdata); // AHB
    }
    else
    {
      /* set bits R + T (bit 15 + bit 14) and clear TM (bit 11) transmit buffer empty */
      raspicomm_spi0_send((SpiConfig = (SpiConfig | MAX3140_WRITE_CONFIG) & ~MAX3140_UART_TM));

      /* give the max3140 enough time to send the data over usart before disabling RTS, else the transmission is broken */
      udelay(SwBacksleep); // AHB Erl�uterung: Microsekunden Verz�gerung: 10.000.000/Baudrate: 9600 --> ca. 1 mSec

      /* enable receive by disabling RTS (TE set so that no data is sent)*/
      raspicomm_spi0_send(MAX3140_WRITE_DATA_R | MAX3140_WRITE_DATA_RTS | MAX3140_WRITE_DATA_TE);
      LOG("raspicomm_irq RTS disabled --> receiving..."); // AHB
    }
  } // RX oder TBE interrupt

  spin_unlock_irqrestore(&dev_lock, flags); // AHB Freigabe des Locks

  return IRQ_HANDLED;
}

#if 0 // +++--
// sets the specified gpio to the alternative function from the argument
static void raspicomm_spi0_init_gpio_alt(int gpio, int alt)
{
  volatile unsigned int* p;
  int address;

  LOG("raspicomm_spi0_init_gpio_alt(gpio=%i, alt=%i) called", gpio, alt);

  // calc the memory address for manipulating the gpio
  address = GPIO_BASE + (4 * (gpio / 10) );

  // map the gpio into kernel memory
  p = ioremap(address, 4);

  // if the mapping was successful
  if (p != NULL) {

    LOG("ioremap returned %X", (int)p );

    // set the gpio to the alternative mapping
    (*p) |= (((alt) <= 3 ? (alt) + 4 : (alt) == 4 ? 3 : 2) << (((gpio)%10)*3));

    // free the gpio mapping again
    iounmap(p);
  }
}

// init the gpios as specified in 
static int raspicomm_spi0_init_gpio()
{
  int i, length = sizeof(GpioConfigs) / sizeof(gpioconfig), ret = SUCCESS;

  LOG ( "raspicomm_spi0_init_gpio() called with %i gpios", length );

  for (i = 0; i < length; i++)
  {
    if ( gpio_request_one( GpioConfigs[i].gpio, GPIOF_IN, "SPI" ) == 0 )
    {
      GpioConfigs[i].gpio_requested = GpioConfigs[i].gpio; // mark the gpio as successfully requested

      LOG ( "gpio_request_one(%i) succeeded", GpioConfigs[i].gpio);

      // set the alternative function according      
      raspicomm_spi0_init_gpio_alt( GpioConfigs[i].gpio, GpioConfigs[i].gpio_alternative );
    }
    else {
      printk( KERN_ERR "raspicomm: gpio_request_one(%i) failed", GpioConfigs[i].gpio );
      ret--;
    }
  }

  return ret;
}

static void raspicomm_spi0_deinit_gpio()
{
  int i, length = sizeof(GpioConfigs) / sizeof(gpioconfig);
 
  LOG( "raspicomm_spi0_deinit_gpio() called" );

  // frees all gpios that we successfully requested  
  for (i = 0; i < length; i++)
  {
    if ( GpioConfigs[i].gpio_requested ) 
    {
      LOG( "Freeing gpio %i", GpioConfigs[i].gpio_requested );

      gpio_free( GpioConfigs[i].gpio_requested );
      GpioConfigs[i].gpio_requested = 0;

    }
  }
}
#endif

static int raspicomm_spi0_init_irq()
{  
  // the interrupt number and gpio number
  int irq, io = 17;

  // request the gpio and configure it as an input
  int err = gpio_request_one(io, GPIOF_IN, "SPI0");

  if (err) {
    printk( KERN_ERR "raspicomm: gpio_request_one(%i) failed with error=%i", io, err );
    return -1;
  } 

  // store the requested gpio so that it can be freed later on
  Gpio = io;

  // map it to an irq
  irq = gpio_to_irq(io);

  if (irq < 0) {
    printk( KERN_ERR "raspicomm: gpio_to_irq failed with error=%i", irq );
    return -1;
  }

  // store the irq so that it can be freed later on
  Gpio17_Irq = irq;

  // request the interrupt
  err = request_irq(irq,                           // the irq we want to receive
                    raspicomm_irq_handler,         // our irq handler function
                    IRQF_TRIGGER_FALLING,          // irq is triggered on the falling edge
                    IRQ_DEV_NAME,                  // device name that is displayed in /proc/interrupts
                    (void*)(raspicomm_irq_handler) // a unique id, needed to free the irq
                   );

  if (err) {
    printk( KERN_ERR "raspicomm: request_irq(%i) failed with error=%i", irq, err);
    return -1;
  }

  LOG ( "raspicomm_spi0_init_irq completed successfully");

  return SUCCESS;
}

static void raspicomm_spi0_deinit_irq()
{
  int gpio = Gpio;
  int irq = Gpio17_Irq;

  // if we've got a valid irq, free it
  if (irq > 0)  {

    // disable the irq first
    LOG( "Disabling irq ");
    disable_irq(irq);

    // free the irq
    LOG( "Freeing irq" );
    free_irq(irq, (void*)(raspicomm_irq_handler));
    Gpio17_Irq = 0;
  }

  // if we've got a valid gpio, free it
  if (gpio > 0) {
    LOG ( "Freeing gpio" );
    gpio_free(gpio);
    Gpio = 0;
  }

}

#if 0 // +++--
// initializes the spi0 using the memory region Spi0
static void raspicomm_spi0_init_port()
{
  // 1 MHz spi clock
  //SPI0_CLKSPEED = 250 / 1;
  SPI0_CLKSPEED = 80;

  // clear FIFOs and all status bits
  SPI0_CNTLSTAT = SPI0_CS_CLRALL;

  SPI0_CNTLSTAT = SPI0_CS_DONE; // make sure done bit is cleared
}

// frees the memory are used to access the spi0
static void raspicomm_spi0_deinit_mem(volatile unsigned int* spi0)
{
  // after using the device call iounmap to return the address space to the kernel
  if (spi0 != NULL)
    iounmap(spi0);
}
#endif

// this function pushes a received character to the opened tty device, called by the interrupt function
static void raspicomm_rs485_received(struct tty_struct* tty, char c)
{
  LOG( "raspicomm_rs485_received(c=%c)", c);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
  if (tty != NULL && tty->port != NULL)
  {
    // send the character to the tty
    tty_insert_flip_char(tty->port, c, TTY_NORMAL);

    // tell it to flip the buffer
    tty_flip_buffer_push(tty->port);
  }
#else
  if (tty != NULL)
  {
    // send the character to the tty
    tty_insert_flip_char(tty, c, TTY_NORMAL);

    // tell it to flip the buffer
    tty_flip_buffer_push(tty);
  }
#endif

}

// ****************************************************************************
// **** END raspicomm private function implementations
// ****************************************************************************


// ****************************************************************************
// **** START tty driver interface function implementations
// ****************************************************************************
// called by the kernel when open() is called for the device
static int raspicommDriver_open(struct tty_struct* tty, struct file* file)
{
  LOG("raspicommDriver_open() called");

  if (OpenCount++)
  {
    LOG( "raspicommDriver_open() was not successful as OpenCount = %i", OpenCount);

    return -ENODEV;
  }
  else
  {
    LOG( "raspicommDriver_open() was successful");

    OpenTTY = tty;

    // TODO Do we need to reset the connection?
    // reset the connection
    // raspicomm_max3140_apply_config();

    return SUCCESS;
  }

}

// called by the kernel when close() is called for the device
static void raspicommDriver_close(struct tty_struct* tty, struct file* file)
{
  LOG("raspicommDriver_close called");

  if (--OpenCount)
  {
    LOG( "device was not closed, as an open count is %i", OpenCount);
  }
  else
  {
    OpenTTY = NULL;
    LOG( "device was closed");
  }
}

static int raspicommDriver_write(struct tty_struct* tty,
  const unsigned char* buf,
  int count)
{
  int bytes_written = 0;

  LOG("raspicommDriver_write(count=%i)\n", count);

  while (bytes_written < count)
  {
    if (queue_enqueue(&TxQueue, buf[bytes_written]))
    {
      bytes_written++;
    }
    else { // kein Platz mehr vorhanden --> schlafen, senden
      cpu_relax();

      raspicomm_start_transfer();
    } // nur falls Platz knapp
  }
  // AHB Sorge daf�r, dass der TBE interrupt auf jeden Fall aktiviert wird (falls nicht bereits oben bei Platzmangel)
  raspicomm_start_transfer();

  return bytes_written;
}

// called by kernel to evaluate how many bytes can be written
static int raspicommDriver_write_room(struct tty_struct *tty)
{
  return INT_MAX;
}

static void raspicommDriver_flush_buffer(struct tty_struct * tty)
{
  LOG("raspicommDriver_flush_buffer called");
}

static int raspicommDriver_chars_in_buffer(struct tty_struct * tty)
{
  //LOG("raspicommDriver_chars_in_buffer called");
  return 0;
}

// called by the kernel when cfsetattr() is called from userspace
static void raspicommDriver_set_termios(struct tty_struct* tty, struct ktermios* kt)
{
  int cflag;
  speed_t baudrate; Databits databits; Parity parity; Stopbits stopbits;

  LOG("raspicommDriver_set_termios() called");

  // get the baudrate
  baudrate = tty_get_baud_rate(tty);

  // get the cflag
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
  cflag = tty->termios.c_cflag;
#else
  cflag = tty->termios->c_cflag;
#endif

  // get the databits
  switch ( cflag & CSIZE )
  {
    case CS7:
      databits = DATABITS_7;
      break;

    default:
    case CS8:
      databits = DATABITS_8;
      break;
  }

  // get the stopbits
  stopbits = ( cflag & CSTOPB ) ? STOPBITS_TWO : STOPBITS_ONE;

  // get the parity
  if ( cflag & PARENB ) // is parity used
  {
    ParityIsEven = !( cflag & PARODD ); // is it even or odd? store it for sending
    parity = PARITY_ON;
    ParityEnabled = 1;
  }
  else {
    parity = PARITY_OFF;
    ParityEnabled = 0;
  }

  // #if DEBUG
  //   printk ( KERN_INFO "raspicomm: Parity=%i, ParityIsEven = %i", parity, ParityIsEven);
  // #endif
  
  // update the configuration
  raspicomm_max3140_configure(baudrate, databits, stopbits, parity);

  raspicomm_max3140_apply_config();
}

static void raspicommDriver_stop(struct tty_struct * tty)
{
  LOG("raspicommDriver_stop called");
}

static void raspicommDriver_start(struct tty_struct * tty)
{
  LOG("raspicommDriver_start called");
}

static void raspicommDriver_hangup(struct tty_struct * tty)
{
  LOG("raspicommDriver_hangup called");
}

static int raspicommDriver_tiocmget(struct tty_struct *tty)
{
  LOG("raspicommDriver_tiocmget called");
  return 0;
}

static int raspicommDriver_tiocmset(struct tty_struct *tty,
                              unsigned int set, 
                              unsigned int clear)
{
  LOG("raspicommDriver_tiocmset called");
  return 0;
}

// called by the kernel to get/set data
static int raspicommDriver_ioctl(struct tty_struct* tty,
                           unsigned int cmd,
                           unsigned int long arg)
{
  int ret;

  // LOG("raspicomm: raspicommDriver_ioctl called");

  LOG ("raspicommDriver_ioctl() called with cmd=%i, arg=%li", cmd, arg);

  switch (cmd)
  {
    case TIOCMSET:
      ret = 0;
      break;

    case TIOCMGET:
      ret = 0;
      break;

    default:
      ret = -ENOIOCTLCMD;
      break;
  }

  return ret;
}

static void raspicommDriver_throttle(struct tty_struct * tty)
{
  LOG_INFO("throttle");
}

static void raspicommDriver_unthrottle(struct tty_struct * tty)
{
  LOG_INFO("unthrottle");
}


// ****************************************************************************
// **** END raspicommDriver interface functions ****
// ****************************************************************************
