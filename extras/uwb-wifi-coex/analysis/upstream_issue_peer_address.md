# Draft issue for Truesense-it/PortentaUWBShield

Not posted yet. Title and body below are ready to paste.

---

**Title:** UWBRangingController / UWBRangingControlee send a dangling PEER_ADDRESS (session start rejected or no response)

**Library:** PortentaUWBShield 1.0.2 (also HEAD 087c689 and 1.0.1), Portenta C33, core arduino:renesas_portenta 1.6.0

### Summary

`UWBRangingController` and `UWBRangingControlee` take `UWBMacAddress dstAddr` by value and call
`appParams.destinationMacAddr(dstAddr)` in the constructor. `UWBAppParamList::destinationMacAddr(UWBMacAddress&)`
stores only a pointer to `addr.getData()` (`buildArray()` keeps `param_value = val`, no copy). When the
constructor returns, `dstAddr` is destroyed, so `init()` later sends PEER_ADDRESS from a dead stack frame.

Source references:
- `src/uwbapps/UWBRangingController.hpp:14,27` and `src/uwbapps/UWBRangingControlee.hpp:14,28`: by-value `dstAddr`, pointer registered
- `src/uwbapps/UWBAppParamList.cpp:15-26` (`buildArray`) and `:121-124` (`destinationMacAddr`)
- `rangingParams.deviceMacAddr()` copies with `memcpy`, so the source address is not affected
- `UWBRangingOneToMany` uses `UWBMacAddressList::getAllData()`, which returns a static buffer, so it is not affected

### Symptoms (two C33 + UWB Shield boards, official 1:1 examples, unmodified)

- `UWB_RangingController`: `UwbApi_StartRangingSession: waitforNotification for event 162 Failed`, then
  `uwb_bus_io_irq_wait: phOsalUwb_ConsumeSemaphore_WithTimeout failed` every 10 s.
  A SessionInfo callback shows SESSION_START response OK followed by SESSION_STATUS_NTF state 0x03 (IDLE)
  with reason 0x8D; another build gave reason 0x35.
- `UWB_RangingControlee`: ranging notifications arrive, but every measurement has status 0x21.
- The result changes between builds because it depends on what is left on the stack.

### Workaround (sketch side)

```cpp
static UWBMacAddress peerKeep(UWBMacAddress::Size::SHORT, peer);
static UWBRangingControlee session(0x11223344, srcAddr, peerKeep);
session.appParams.destinationMacAddr(peerKeep);   // re-register from a static object
UWBSessionManager.addSession(session);
session.init();
session.start();
```

With this on both boards (1:1 Controller + Controlee), ranging works: 214 valid distances on the controller
and 235 on the controlee in 45 s.

### Suggested fix

Copy the address into the session instead of keeping a pointer, for example store a `UWBMacAddress` member
in `UWBRangingController` / `UWBRangingControlee` (or in `UWBAppParamList`) and register that member, or make
`buildArray()` users own their buffers.
