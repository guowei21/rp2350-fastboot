/*********************************************************************
 Adafruit invests time and resources providing this open source code,
 please support Adafruit and open-source hardware by purchasing
 products from Adafruit!

 MIT license, check LICENSE for more information
 Copyright (c) 2019 Ha Thach for Adafruit Industries
 All text above, and the splash screen below must be included in
 any redistribution
*********************************************************************/

// pio-usb is required for rp2040 host
#include "pio_usb.h"
#define HOST_PIN_DP   2    // RP2350-USB-A: D+ = GPIO2, D- = GPIO3

#include "Adafruit_TinyUSB.h"

#define LANGUAGE_ID 0x0409  // English

#define FIRMWARE_VERSION "rp2350-fastboot-20260711-color-fallback"


#define REBOOT_CMD "oem set-gpu-preemption 0 androidboot.selinux=permissive\x00"
#define REBOOT_CMD_LEN (sizeof(REBOOT_CMD) - 1)

#define REBOOT_CMD2 "continue\x00"
#define REBOOT_CMD2_LEN (sizeof(REBOOT_CMD2) - 1)

// USB Host object
Adafruit_USBH_Host USBHost;

// holding device descriptor
tusb_desc_device_t desc_device;

enum FastbootState : uint8_t {
  FB_DISCONNECTED,
  FB_READY_FIRST,
  FB_FIRST_RUNNING,
  FB_READY_SECOND,
  FB_SECOND_RUNNING
};

static volatile FastbootState fb_state = FB_DISCONNECTED;
static volatile bool success_flash_requested = false;

// Transfer state must remain valid until TinyUSB invokes the completion callback.
static tuh_xfer_t xfer;
static uint8_t reboot_cmd[] = REBOOT_CMD;
static uint8_t reboot_cmd2[] = REBOOT_CMD2;
static uint8_t fastboot_dev_addr = 0;
static uint8_t fastboot_ep_addr = 0;
static volatile bool command_in_flight = false;


#include <Adafruit_NeoPixel.h>
#ifdef __AVR__
 #include <avr/power.h> // Required for 16 MHz Adafruit Trinket
#endif

// Which pin on the Arduino is connected to the NeoPixels?
#define PIN        16 // On Trinket or Gemma, suggest changing this to 1

// How many NeoPixels are attached to the Arduino?
#define NUMPIXELS 1

// When setting up the NeoPixel library, we tell it how many pixels,
// and which pin to use to send signals. Note that for older NeoPixel
// strips you might need to change the third parameter -- see the
// strandtest example for more information on possible values.
// Waveshare RP2350-USB-A onboard WS2812 uses RGB byte order with Adafruit_NeoPixel.
Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_RGB + NEO_KHZ800);

static uint32_t state_color(FastbootState state)
{
  switch (state) {
    case FB_READY_FIRST:    return pixels.Color(0, 0, 255);
    case FB_FIRST_RUNNING:  return pixels.Color(255, 160, 0);
    case FB_READY_SECOND:   return pixels.Color(255, 255, 0);
    case FB_SECOND_RUNNING: return pixels.Color(180, 0, 255);
    default:                return pixels.Color(255, 0, 0);
  }
}

static void update_status_led()
{
  static FastbootState shown_state = (FastbootState) 0xff;
  static bool flashing = false;
  static bool flash_on = false;
  static uint8_t transitions_left = 0;
  static uint32_t next_transition_ms = 0;

  if (success_flash_requested) {
    success_flash_requested = false;
    flashing = true;
    flash_on = true;
    transitions_left = 6;
    next_transition_ms = millis() + 150;
    pixels.setPixelColor(0, pixels.Color(255, 255, 255));
    pixels.show();
  }

  if (flashing && (int32_t)(millis() - next_transition_ms) >= 0) {
    flash_on = !flash_on;
    pixels.setPixelColor(0, flash_on ? pixels.Color(255, 255, 255) : 0);
    pixels.show();
    next_transition_ms += 150;
    if (--transitions_left == 0) {
      flashing = false;
      shown_state = (FastbootState) 0xff;
    }
  }

  FastbootState current = fb_state;
  if (!flashing && current != shown_state) {
    shown_state = current;
    pixels.setPixelColor(0, state_color(current));
    pixels.show();
  }
}




// the setup function runs once when you press reset or power the board
void setup()
{
  Serial1.begin(115200);

  Serial.begin(115200);
  //while ( !Serial ) delay(10);   // wait for native usb

  Serial.println("TinyUSB Fastboot Host");
  Serial.println(FIRMWARE_VERSION);


  // These lines are specifically to support the Adafruit Trinket 5V 16 MHz.
  // Any other board, you can remove this part (but no harm leaving it):
#if defined(__AVR_ATtiny85__) && (F_CPU == 16000000)
  clock_prescale_set(clock_div_1);
#endif
  // END of Trinket-specific code.

  pixels.begin(); // INITIALIZE NeoPixel strip object (REQUIRED)
  Serial.printf("READY! %s\n", FIRMWARE_VERSION);
  Serial1.printf("READY! %s\n", FIRMWARE_VERSION);
  pixels.clear(); // Set all pixel colors to 'off'
  pixels.setPixelColor(0, pixels.Color(255, 0, 0));// Red1	255 0 0	#FF0000  --green
  pixels.show();   // Send the updated pixel colors to the hardware.
}

void loop()
{
  static bool last_button = false;
  static uint32_t last_press_ms = 0;
  static uint32_t next_button_poll_ms = 0;

  update_status_led();
  if ((int32_t)(millis() - next_button_poll_ms) < 0) return;
  next_button_poll_ms = millis() + 10;

  bool button = BOOTSEL;

  if (button && !last_button && millis() - last_press_ms >= 250) {
    last_press_ms = millis();
    FastbootState current = fb_state;
    if (current == FB_READY_FIRST) {
      send_fastboot_reboot(fastboot_dev_addr, fastboot_ep_addr);
    } else if (current == FB_READY_SECOND) {
      send_fastboot_reboot2(fastboot_dev_addr, fastboot_ep_addr);
    }
  }
  last_button = button;

}

// core1's setup
void setup1() {
  //while ( !Serial ) delay(10);   // wait for native usb
  Serial.println("Core1 setup to run TinyUSB host with pio-usb");

  // Check for CPU frequency, must be multiple of 120Mhz for bit-banging USB
  uint32_t cpu_hz = clock_get_hz(clk_sys);
  if ( cpu_hz != 120000000UL && cpu_hz != 240000000UL ) {
    while ( !Serial ) delay(10);   // wait for native usb
    Serial.printf("Error: CPU Clock = %u, PIO USB require CPU clock must be multiple of 120 Mhz\r\n", cpu_hz);
    Serial1.printf("Error: CPU Clock = %u, PIO USB require CPU clock must be multiple of 120 Mhz\r\n", cpu_hz);
    Serial.printf("Change your CPU Clock to either 120 or 240 Mhz in Menu->CPU Speed \r\n", cpu_hz);
    Serial1.printf("Change your CPU Clock to either 120 or 240 Mhz in Menu->CPU Speed \r\n", cpu_hz);
    while(1) delay(1);
  }

  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  pio_cfg.pin_dp = HOST_PIN_DP;
 
 #if defined(ARDUINO_RASPBERRY_PI_PICO_W)
  /* https://github.com/sekigon-gonnoc/Pico-PIO-USB/issues/46 */
  pio_cfg.sm_tx      = 3;
  pio_cfg.sm_rx      = 2;
  pio_cfg.sm_eop     = 3;
  pio_cfg.pio_rx_num = 0;
  pio_cfg.pio_tx_num = 1;
  pio_cfg.tx_ch      = 9;
 #endif /* ARDUINO_RASPBERRY_PI_PICO_W */
 
  USBHost.configure_pio_usb(1, &pio_cfg);

  // run host stack on controller (rhport) 1
  // Note: For rp2040 pico-pio-usb, calling USBHost.begin() on core1 will have most of the
  // host bit-banging processing works done in core1 to free up core0 for other works
  USBHost.begin(1);
}

// core1's loop
void loop1()
{
  USBHost.task();
}

//--------------------------------------------------------------------+
// TinyUSB Host callbacks
//--------------------------------------------------------------------+

// Callback when a device is mounted (connected)
void tuh_mount_cb(uint8_t dev_addr)
{
  fb_state = FB_DISCONNECTED;
  command_in_flight = false;
  fastboot_dev_addr = dev_addr;
  fastboot_ep_addr = 0;

  Serial.printf("Device attached, address = %d\r\n", dev_addr);
  Serial1.printf("Device attached, address = %d\r\n", dev_addr);
  // Get Device Descriptor
  // tuh_descriptor_get_device(dev_addr, &desc_device, 18, print_device_descriptor, 0);

  // get device descriptor
  static uint8_t desc[512]; // Big enough for device descriptor
  // store result of tuh_descriptor_get_configuration to check if it is successful
  bool result = tuh_descriptor_get_configuration(dev_addr, 0, desc, sizeof(desc), descriptor_complete_cb, 0);
  if (!result)
  {
      Serial.printf("Failed to retrieve configuration descriptor\n");
      Serial1.printf("Failed to retrieve configuration descriptor\n");
  }

}

// Callback when a device mount is failed
void tuh_mount_failed_cb(uint8_t dev_addr)
{
    Serial.printf("Device mount failed, address = %d\n", dev_addr);
    Serial1.printf("Device mount failed, address = %d\n", dev_addr);
}

// Callback when a device is unmounted (disconnected)
void tuh_umount_cb(uint8_t dev_addr)
{
    Serial.printf("Device removed, address = %d\n", dev_addr);
    Serial1.printf("Device removed, address = %d\n", dev_addr);
    command_in_flight = false;
    fastboot_dev_addr = 0;
    fastboot_ep_addr = 0;
    fb_state = FB_DISCONNECTED;
}

// Callback when a descriptor is retrieved
void descriptor_complete_cb(tuh_xfer_t *xfer)
{
    if (xfer->result != XFER_RESULT_SUCCESS) {
        fb_state = FB_DISCONNECTED;
        return;
    }

    // Store the descriptor data in a local buffer
    uint8_t *desc = (uint8_t *)xfer->buffer;

    // First, log the descriptor data for debugging
    Serial.printf("Descriptor data:\n");
    Serial1.printf("Descriptor data:\n");
    for (int i = 0; i < xfer->actual_len; i++)
    {
        Serial.printf("%02x ", desc[i]);
        Serial1.printf("%02x ", desc[i]);
    }
    Serial.printf("\n");
    Serial1.printf("\n");

    // Second, find the bulk OUT endpoint.
    // Prefer Android Fastboot interface FF/42/03, but some phones/bootloaders
    // expose a vendor-specific bulk interface with a different subclass/protocol.
    uint8_t dev_addr = xfer->daddr;
    uint8_t ep_addr = 0; // Initialize to 0, meaning not found.
    uint8_t *end = desc + xfer->actual_len;
    const tusb_desc_endpoint_t *ep_desc = nullptr;
    const tusb_desc_endpoint_t *fallback_ep_desc = nullptr;
    uint8_t fallback_ep_addr = 0;
    bool exact_fastboot_interface = false;
    bool vendor_bulk_interface = false;

    // Iterate through the descriptor to find the bulk OUT endpoint
    while (end - desc >= 2)
    {
        uint8_t descriptor_len = desc[0];
        if (descriptor_len < 2 || descriptor_len > end - desc) break;

        if (desc[1] == TUSB_DESC_INTERFACE && descriptor_len >= 9)
        {
            // Android Fastboot interface: vendor class, subclass 0x42, protocol 0x03.
            exact_fastboot_interface = desc[5] == 0xff && desc[6] == 0x42 && desc[7] == 0x03;
            vendor_bulk_interface = desc[5] == 0xff;
            Serial.printf("Interface found: class = %d, subclass = %d, protocol = %d\n", desc[5], desc[6], desc[7]);
            Serial1.printf("Interface found: class = %d, subclass = %d, protocol = %d\n", desc[5], desc[6], desc[7]);
        }
        else if (vendor_bulk_interface && desc[1] == TUSB_DESC_ENDPOINT && descriptor_len >= sizeof(tusb_desc_endpoint_t))
        {
            // Found an endpoint descriptor
            uint8_t endpoint_address = desc[2];
            uint8_t attributes = desc[3];

            // If this is a BULK OUT endpoint, set ep_addr and break
            if ((endpoint_address & TUSB_DIR_IN_MASK) == 0 &&
                (attributes & 0x03) == TUSB_XFER_BULK)
            {
                if (exact_fastboot_interface) {
                    ep_addr = endpoint_address;
                    ep_desc = (const tusb_desc_endpoint_t*)desc;
                    Serial.printf("Exact Fastboot Bulk OUT endpoint found at address 0x%02x\n", ep_addr);
                    Serial1.printf("Exact Fastboot Bulk OUT endpoint found at address 0x%02x\n", ep_addr);
                    break;
                } else if (!fallback_ep_desc) {
                    fallback_ep_addr = endpoint_address;
                    fallback_ep_desc = (const tusb_desc_endpoint_t*)desc;
                    Serial.printf("Vendor Bulk OUT fallback endpoint found at address 0x%02x\n", fallback_ep_addr);
                    Serial1.printf("Vendor Bulk OUT fallback endpoint found at address 0x%02x\n", fallback_ep_addr);
                }
            }
        }

        // Move to the next descriptor in the configuration descriptor
        desc += descriptor_len;
    }

    if (!ep_desc && fallback_ep_desc) {
        ep_addr = fallback_ep_addr;
        ep_desc = fallback_ep_desc;
        Serial.printf("Using vendor Bulk OUT fallback endpoint 0x%02x\n", ep_addr);
        Serial1.printf("Using vendor Bulk OUT fallback endpoint 0x%02x\n", ep_addr);
    }

    if (ep_addr != 0 && ep_desc)
    {
        // Attempt to open the endpoint
        if (!tuh_edpt_open(dev_addr, ep_desc))
        {
            Serial.printf("Failed to open Bulk OUT endpoint at address 0x%02x\n", ep_addr);
            Serial1.printf("Failed to open Bulk OUT endpoint at address 0x%02x\n", ep_addr);
            fb_state = FB_DISCONNECTED;
            return;
        }

        fastboot_dev_addr = dev_addr;
        fastboot_ep_addr = ep_addr;
        fb_state = FB_READY_FIRST;
        Serial.printf("Bulk OUT endpoint opened at address 0x%02x\n", ep_addr);
        Serial1.printf("Bulk OUT endpoint opened at address 0x%02x\n", ep_addr);
        Serial.printf("Fastboot ready; press BOOTSEL for command 1.\n");
        Serial1.printf("Fastboot ready; press BOOTSEL for command 1.\n");

    }
    else
    {
        Serial.printf("Bulk OUT endpoint not found.\n");
        Serial1.printf("Bulk OUT endpoint not found.\n");
    }
}

// Send the fastboot reboot command to the device
void send_fastboot_reboot(uint8_t dev_addr, uint8_t ep_addr)
{
    if (command_in_flight || fb_state != FB_READY_FIRST || !dev_addr || !ep_addr) return;

    fastboot_dev_addr = dev_addr;
    fastboot_ep_addr = ep_addr;

    // Initialize the transfer structure
    xfer.daddr = dev_addr;                   // Device address
    xfer.ep_addr = ep_addr;                  // Endpoint address
    xfer.result = static_cast<xfer_result_t>(0);                         // Initialize result, may be updated by the transfer
    xfer.actual_len = 0;                     // Actual length of data transferred, initialized to 0
    xfer.buffer = reboot_cmd;                // Pointer to the persistent data buffer
    xfer.buflen = REBOOT_CMD_LEN;            // Length of the data buffer
    xfer.complete_cb = transfer_complete_cb; // Callback function
    xfer.user_data = 0;                      // User data, initialized to 0

    // Send the command using tuh_edpt_xfer
    fb_state = FB_FIRST_RUNNING;
    command_in_flight = true;
    if (tuh_edpt_xfer(&xfer))
    {
        Serial.printf("Fastboot reboot command sent to device %d's endpoint %d\n", dev_addr, ep_addr);
        Serial1.printf("Fastboot reboot command sent to device %d's endpoint %d\n", dev_addr, ep_addr);
    }
    else
    {
        command_in_flight = false;
        fb_state = FB_READY_FIRST;
        Serial.printf("Failed to send Fastboot reboot command to device %d's endpoint %d. Data:\n", dev_addr, ep_addr);
        Serial1.printf("Failed to send Fastboot reboot command to device %d's endpoint %d. Data:\n", dev_addr, ep_addr);
        // Print the structure for debugging
        Serial.printf("daddr: %d, ep_addr: %d, result: %d, actual_len: %d, buffer: %s, buflen: %d\n", xfer.daddr, xfer.ep_addr, xfer.result, xfer.actual_len, (char *)xfer.buffer, xfer.buflen);
        Serial1.printf("daddr: %d, ep_addr: %d, result: %d, actual_len: %d, buffer: %s, buflen: %d\n", xfer.daddr, xfer.ep_addr, xfer.result, xfer.actual_len, (char *)xfer.buffer, xfer.buflen);
    }
}

// Send the fastboot reboot command to the device
void send_fastboot_reboot2(uint8_t dev_addr, uint8_t ep_addr)
{
    if (command_in_flight || fb_state != FB_READY_SECOND || !dev_addr || !ep_addr) return;

    // Initialize the transfer structure
    xfer.daddr = dev_addr;                   // Device address
    xfer.ep_addr = ep_addr;                  // Endpoint address
    xfer.result = static_cast<xfer_result_t>(0);                         // Initialize result, may be updated by the transfer
    xfer.actual_len = 0;                     // Actual length of data transferred, initialized to 0
    xfer.buffer = reboot_cmd2;               // Pointer to the persistent data buffer
    xfer.buflen = REBOOT_CMD2_LEN;            // Length of the data buffer
    xfer.complete_cb = transfer_complete_cb2; // Callback function
    xfer.user_data = 0;                      // User data, initialized to 0

    // Send the command using tuh_edpt_xfer
    fb_state = FB_SECOND_RUNNING;
    command_in_flight = true;
    if (tuh_edpt_xfer(&xfer))
    {
        Serial.printf("Fastboot2 reboot command sent to device %d's endpoint %d\n", dev_addr, ep_addr);
        Serial1.printf("Fastboot2 reboot command sent to device %d's endpoint %d\n", dev_addr, ep_addr);
    }
    else
    {
        command_in_flight = false;
        fb_state = FB_READY_SECOND;
        Serial.printf("Failed to send Fastboot2 reboot command to device %d's endpoint %d. Data:\n", dev_addr, ep_addr);
        Serial1.printf("Failed to send Fastboot2 reboot command to device %d's endpoint %d. Data:\n", dev_addr, ep_addr);
        // Print the structure for debugging
        Serial.printf("2daddr: %d, ep_addr: %d, result: %d, actual_len: %d, buffer: %s, buflen: %d\n", xfer.daddr, xfer.ep_addr, xfer.result, xfer.actual_len, (char *)xfer.buffer, xfer.buflen);
        Serial1.printf("2daddr: %d, ep_addr: %d, result: %d, actual_len: %d, buffer: %s, buflen: %d\n", xfer.daddr, xfer.ep_addr, xfer.result, xfer.actual_len, (char *)xfer.buffer, xfer.buflen);
    }
}

// Callback when the command is sent
void transfer_complete_cb(tuh_xfer_t *xfer)
{
    command_in_flight = false;
    Serial.printf("Fastboot reboot completed. Result: %d, actual_len: %d\n", xfer->result, xfer->actual_len);
    Serial1.printf("Fastboot reboot completed. Result: %d, actual_len: %d\n", xfer->result, xfer->actual_len);
    if (xfer->result != XFER_RESULT_SUCCESS)
    {
        Serial.printf("Transfer failed with error code %d\n", xfer->result);
        Serial1.printf("Transfer failed with error code %d\n", xfer->result);
        fb_state = fastboot_ep_addr ? FB_READY_FIRST : FB_DISCONNECTED;
        return;
    }

    fb_state = FB_READY_SECOND;
    success_flash_requested = true;
    Serial.printf("Command 1 complete; press BOOTSEL for command 2.\n");
    Serial1.printf("Command 1 complete; press BOOTSEL for command 2.\n");
}

// Callback when the command is sent
void transfer_complete_cb2(tuh_xfer_t *xfer)
{
    command_in_flight = false;
    Serial.printf("Fastboot2 reboot completed. Result: %d, actual_len: %d\n", xfer->result, xfer->actual_len);
    Serial1.printf("Fastboot2 reboot completed. Result: %d, actual_len: %d\n", xfer->result, xfer->actual_len);
    if (xfer->result != XFER_RESULT_SUCCESS)
    {
        Serial.printf("Transfer2 failed with error code %d\n", xfer->result);
        Serial1.printf("Transfer2 failed with error code %d\n", xfer->result);
        fb_state = fastboot_ep_addr ? FB_READY_SECOND : FB_DISCONNECTED;
        return;
    }

    fb_state = fastboot_ep_addr ? FB_READY_FIRST : FB_DISCONNECTED;
    success_flash_requested = true;
    Serial.printf("Command 2 complete; command sequence reset.\n");
    Serial1.printf("Command 2 complete; command sequence reset.\n");
}



void print_device_descriptor(tuh_xfer_t* xfer)
{
  if ( XFER_RESULT_SUCCESS != xfer->result )
  {
    Serial.printf("Failed to get device descriptor\r\n");
	  Serial1.printf("Failed to get device descriptor\r\n");
    return;
  }

  uint8_t const daddr = xfer->daddr;

  Serial.printf("Device %u: ID %04x:%04x\r\n", daddr, desc_device.idVendor, desc_device.idProduct);
  Serial.printf("Device Descriptor:\r\n");
  Serial.printf("  bLength             %u\r\n"     , desc_device.bLength);
  Serial.printf("  bDescriptorType     %u\r\n"     , desc_device.bDescriptorType);
  Serial.printf("  bcdUSB              %04x\r\n"   , desc_device.bcdUSB);
  Serial.printf("  bDeviceClass        %u\r\n"     , desc_device.bDeviceClass);
  Serial.printf("  bDeviceSubClass     %u\r\n"     , desc_device.bDeviceSubClass);
  Serial.printf("  bDeviceProtocol     %u\r\n"     , desc_device.bDeviceProtocol);
  Serial.printf("  bMaxPacketSize0     %u\r\n"     , desc_device.bMaxPacketSize0);
  Serial.printf("  idVendor            0x%04x\r\n" , desc_device.idVendor);
  Serial.printf("  idProduct           0x%04x\r\n" , desc_device.idProduct);
  Serial.printf("  bcdDevice           %04x\r\n"   , desc_device.bcdDevice);
  
  
  Serial1.printf("Device %u: ID %04x:%04x\r\n", daddr, desc_device.idVendor, desc_device.idProduct);
  Serial1.printf("Device Descriptor:\r\n");
  Serial1.printf("  bLength             %u\r\n"     , desc_device.bLength);
  Serial1.printf("  bDescriptorType     %u\r\n"     , desc_device.bDescriptorType);
  Serial1.printf("  bcdUSB              %04x\r\n"   , desc_device.bcdUSB);
  Serial1.printf("  bDeviceClass        %u\r\n"     , desc_device.bDeviceClass);
  Serial1.printf("  bDeviceSubClass     %u\r\n"     , desc_device.bDeviceSubClass);
  Serial1.printf("  bDeviceProtocol     %u\r\n"     , desc_device.bDeviceProtocol);
  Serial1.printf("  bMaxPacketSize0     %u\r\n"     , desc_device.bMaxPacketSize0);
  Serial1.printf("  idVendor            0x%04x\r\n" , desc_device.idVendor);
  Serial1.printf("  idProduct           0x%04x\r\n" , desc_device.idProduct);
  Serial1.printf("  bcdDevice           %04x\r\n"   , desc_device.bcdDevice);

  // Get String descriptor using Sync API
  uint16_t temp_buf[128];

  Serial.printf("  iManufacturer       %u     "     , desc_device.iManufacturer);
  Serial1.printf("  iManufacturer       %u     "     , desc_device.iManufacturer);
  if (XFER_RESULT_SUCCESS == tuh_descriptor_get_manufacturer_string_sync(daddr, LANGUAGE_ID, temp_buf, sizeof(temp_buf)) )
  {
    print_utf16(temp_buf, TU_ARRAY_SIZE(temp_buf));
  }
  Serial.printf("\r\n");
  Serial1.printf("\r\n");

  Serial.printf("  iProduct            %u     "     , desc_device.iProduct);
  Serial1.printf("  iProduct            %u     "     , desc_device.iProduct);
  if (XFER_RESULT_SUCCESS == tuh_descriptor_get_product_string_sync(daddr, LANGUAGE_ID, temp_buf, sizeof(temp_buf)))
  {
    print_utf16(temp_buf, TU_ARRAY_SIZE(temp_buf));
  }
  Serial.printf("\r\n");
  Serial1.printf("\r\n");

  Serial.printf("  iSerialNumber       %u     "     , desc_device.iSerialNumber);
  Serial1.printf("  iSerialNumber       %u     "     , desc_device.iSerialNumber);
  if (XFER_RESULT_SUCCESS == tuh_descriptor_get_serial_string_sync(daddr, LANGUAGE_ID, temp_buf, sizeof(temp_buf)))
  {
    print_utf16(temp_buf, TU_ARRAY_SIZE(temp_buf));
  }
  Serial.printf("\r\n");
  Serial1.printf("\r\n");

  Serial.printf("  bNumConfigurations  %u\r\n"     , desc_device.bNumConfigurations);
  Serial1.printf("  bNumConfigurations  %u\r\n"     , desc_device.bNumConfigurations);
}

//--------------------------------------------------------------------+
// String Descriptor Helper
//--------------------------------------------------------------------+

static void _convert_utf16le_to_utf8(const uint16_t *utf16, size_t utf16_len, uint8_t *utf8, size_t utf8_len) {
  // TODO: Check for runover.
  (void)utf8_len;
  // Get the UTF-16 length out of the data itself.

  for (size_t i = 0; i < utf16_len; i++) {
    uint16_t chr = utf16[i];
    if (chr < 0x80) {
      *utf8++ = chr & 0xff;
    } else if (chr < 0x800) {
      *utf8++ = (uint8_t)(0xC0 | (chr >> 6 & 0x1F));
      *utf8++ = (uint8_t)(0x80 | (chr >> 0 & 0x3F));
    } else {
      // TODO: Verify surrogate.
      *utf8++ = (uint8_t)(0xE0 | (chr >> 12 & 0x0F));
      *utf8++ = (uint8_t)(0x80 | (chr >> 6 & 0x3F));
      *utf8++ = (uint8_t)(0x80 | (chr >> 0 & 0x3F));
    }
    // TODO: Handle UTF-16 code points that take two entries.
  }
}

// Count how many bytes a utf-16-le encoded string will take in utf-8.
static int _count_utf8_bytes(const uint16_t *buf, size_t len) {
  size_t total_bytes = 0;
  for (size_t i = 0; i < len; i++) {
    uint16_t chr = buf[i];
    if (chr < 0x80) {
      total_bytes += 1;
    } else if (chr < 0x800) {
      total_bytes += 2;
    } else {
      total_bytes += 3;
    }
    // TODO: Handle UTF-16 code points that take two entries.
  }
  return total_bytes;
}

static void print_utf16(uint16_t *temp_buf, size_t buf_len) {
  size_t utf16_len = ((temp_buf[0] & 0xff) - 2) / sizeof(uint16_t);
  size_t utf8_len = _count_utf8_bytes(temp_buf + 1, utf16_len);

  _convert_utf16le_to_utf8(temp_buf + 1, utf16_len, (uint8_t *) temp_buf, sizeof(uint16_t) * buf_len);
  ((uint8_t*) temp_buf)[utf8_len] = '\0';

  Serial.printf((char*)temp_buf);
  Serial1.printf((char*)temp_buf);
}

