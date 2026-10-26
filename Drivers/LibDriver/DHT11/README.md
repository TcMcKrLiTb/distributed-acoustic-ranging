# LibDriver DHT11

- Upstream: <https://github.com/libdriver/dht11>
- Revision: `4e68f7c7d31e871413ed4aa4c2f42748193a061f`
- Driver version: 2.0.0
- License: MIT, retained in `LICENSE` and the source headers.
- Imported: `src/driver_dht11.c`, `src/driver_dht11.h`,
  `interface/driver_dht11_interface.h`, `LICENSE`.

Local changes to `driver_dht11.c` / `driver_dht11.h`:

1. Fail when either bit-level wait reaches its retry limit. Upstream continued
   decoding after a timeout, allowing a stuck-low payload to look like a valid
   all-zero frame.
2. Compare the checksum modulo 256 in all three read APIs. This also supports
   frames whose humidity and signed-temperature bytes sum to more than 255.
3. Add the optional `read_frame(uint8_t data[5])` handle callback and
   `DRIVER_DHT11_LINK_READ_FRAME` macro. This lets the STM32 port acquire the
   response with EXTI edge timestamps while tasks and hardware interrupts keep
   running. The callback owns the start pulse, response and five wire bytes;
   this driver still owns checksum validation and value conversion. With a
   NULL callback, the existing GPIO polling and interrupt callbacks are used.
   Zero-initialize the handle with `DRIVER_DHT11_LINK_INIT` before linking it.
   Rebuild users of this locally extended handle; its binary size has changed.

The STM32/FreeRTOS interface and application task are in `App/DHT11/`.
See that directory's README for ownership, timing and board validation.
