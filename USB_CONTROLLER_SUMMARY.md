# Deluge USB Controller Firmware - Integration Summary

## What We've Built

Successfully transformed the Deluge synthesizer into a USB composite device with three interfaces:

### 1. **CDC (USB Serial) Interface**
- **Purpose**: Hardware event streaming (buttons, encoders, pads)
- **Implementation**: `src/controller/usb_serial.c`
- **Protocol**: Binary protocol for sending hardware state changes
- **Status**: ✅ Core structure implemented

### 2. **USB MIDI Interface**
- **Purpose**: MIDI I/O routing
- **Implementation**: `src/controller/usb_midi.c`
- **Features**:
  - Bidirectional MIDI streaming
  - Routes between Deluge's UART MIDI and USB MIDI
- **Status**: ✅ Framework implemented, ready for UART integration

### 3. **USB Audio Class 2.0 Interface**
- **Purpose**: Stereo audio I/O
- **Implementation**: `src/controller/usb_audio.c`
- **Specifications**:
  - **Sample Rate**: 44.1kHz
  - **Bit Depth**: 16-bit
  - **Channels**: 2 (stereo)
  - **Input**: Mic/Line input from Deluge → Host
  - **Output**: Host audio → Deluge speakers/output
- **Status**: ✅ Framework implemented, ready for SSI/ADC integration

## USB Descriptors

Complete composite device descriptor in `src/controller/usb_descriptors.c`:
- Device class: MISC with IAD (Interface Association Descriptor)
- VID: 0x16D0 (MCS Electronics - for open source)
- PID: 0x0EDA (Deluge Controller)
- Configuration: CDC + Audio + MIDI interfaces

Detailed audio descriptor in `src/controller/usb_descriptors.h`:
- Audio Control interface with clock source
- Audio Streaming interface for speaker output (USB → Deluge)
- Audio Streaming interface for mic/line input (Deluge → USB)
- Feature units for volume/mute control

## TinyUSB Configuration

`src/controller/tusb_config.h` configured with:
- **MCU**: OPT_MCU_RZA1X (Renesas RZA1L)
- **USB Controller**: RUSB1 (48MHz USB_X1 clock)
- **Device Classes**: CDC, MIDI, Audio (UAC2)
- **Audio Buffer Sizes**: Optimized for 44.1kHz streaming
- **MIDI Buffers**: 128 bytes TX/RX

## Build System

Updated CMake configuration:
- `lib/CMakeLists.txt`: Added TinyUSB library with audio and MIDI device classes
- `src/controller/CMakeLists.txt`: Added usb_audio.c and usb_midi.c

TinyUSB sources now included:
- Core: tusb.c, usbd.c, usbd_control.c
- CDC: cdc_device.c
- MIDI: midi_device.c
- Audio: audio_device.c
- Driver: dcd_rusb1.c, rusb1_common.c (RZA1 USB peripheral)

## Current Firmware Size

```
   text    data     bss     dec     hex filename
  31728    2716  104872  139316   22034 deluge.elf
```

- Code: 31KB
- Initialized Data: 3KB
- BSS (uninitialized): 104KB
- **Total**: 139KB

## Next Steps for Full Integration

### Audio Integration
1. **Connect to SSI (Serial Sound Interface)**:
   - `src/RZA1/ssi/ssi.c` - SSI audio codec interface
   - Hook `tud_audio_tx_done_pre_load_cb()` to read from SSI input
   - Hook `tud_audio_rx_done_post_read_cb()` to write to SSI output

2. **ADC/DAC Routing**:
   - Connect Deluge's ADC (analog input) to USB Audio IN endpoint
   - Connect USB Audio OUT endpoint to Deluge's DAC (speaker output)

### MIDI Integration
1. **UART MIDI Routing**:
   - Hook `src/deluge/drivers/uart/uart.c` MIDI functions
   - Read from UART MIDI → Send via `tud_midi_stream_write()`
   - Receive from USB MIDI → Send to UART MIDI

2. **MIDI Protocol**:
   - Parse/generate standard MIDI messages
   - Handle MIDI clock, SysEx, etc.

### Hardware Events
Current implementation in `src/controller/hardware_events.c`:
- Button scanning
- Encoder reading
- Pad matrix scanning
- LED control (for visual feedback)

## Testing Plan

1. **Enumeration Test**:
   - Connect to PC/Mac
   - Verify composite device with 3 interfaces appears
   - Check device descriptors with `lsusb -v` (Linux) or USBDeview (Windows)

2. **CDC Test**:
   - Open serial port
   - Press buttons/turn encoders on Deluge
   - Verify events stream to host

3. **MIDI Test**:
   - Use MIDI monitor software
   - Send MIDI from Deluge MIDI IN
   - Verify appears on USB MIDI
   - Send from host → verify on Deluge MIDI OUT

4. **Audio Test**:
   - Configure as audio device in OS
   - Play audio from host → verify on Deluge speakers
   - Speak into Deluge mic → verify audio appears on host
   - Check sample rate (44.1kHz) and bit depth (16-bit)

## Files Created/Modified

### New Files:
- `src/controller/usb_audio.c` - Audio streaming implementation
- `src/controller/usb_audio.h` - Audio API header
- `src/controller/usb_midi.c` - MIDI routing implementation
- `src/controller/usb_midi.h` - MIDI API header
- `src/controller/usb_descriptors.h` - USB descriptor definitions

### Modified Files:
- `src/controller/controller.c` - Main loop integration
- `src/controller/CMakeLists.txt` - Build configuration
- `src/controller/tusb_config.h` - TinyUSB configuration
- `src/controller/usb_descriptors.c` - Composite descriptors
- `lib/CMakeLists.txt` - Added MIDI/Audio device classes
- `src/main.c` - Weak declaration for initUartDMA
- `src/malloc.c` - Weak declarations for memory allocator
- `src/terminate.cpp` - Weak declaration for freezeWithError
- `src/OSLikeStuff/fault_handler/fault_handler.c` - Weak declarations for UART
- `src/RZA1/intc/intc_handler.c` - Weak declaration for uartPrintln

## Key Technical Decisions

1. **Weak Linking**: Used weak symbols for deluge-specific functions to allow controller mode to compile without full deluge dependencies

2. **Audio Format**: 44.1kHz 16-bit stereo matches Deluge's native audio processing

3. **Composite Device**: Single USB device with multiple interfaces provides better compatibility than multiple USB devices

4. **TinyUSB**: Modern, well-maintained USB stack with good UAC2 support

5. **Buffer Sizes**: Calculated for approximately 1ms latency at 44.1kHz

## Resources

- TinyUSB Documentation: https://docs.tinyusb.org/
- USB Audio Class 2.0 Spec: USB.org
- RZA1 RUSB1 Controller: Renesas documentation
- Deluge Hardware: https://synthstrom.com/

---

**Status**: ✅ **BUILD SUCCESSFUL** - Framework complete, ready for hardware integration
