# Q25 code review — 2026-11-22

Overnight review of the whole port, with emphasis on code inherited from the
earlier agent: kernel patches + DT + config, the ported vendor drivers
(memory safety / lifetime), userspace + build + the ModemManager plugin, and
the GPU stack. Findings were checked against the code before acting; this file
records what was fixed and what is still open.

## Fixed

| Area | Finding | Fix |
|---|---|---|
| Kernel modules on the phone | 405 in-tree modules were from an older kernel config. No MODVERSIONS, so they still loaded; after #45 enabled XFRM (`struct net` grew) `tun.ko` wrote at wrong offsets → oops in `tun_chr_close` at the same address every boot, 5-minute shutdown hangs, slab corruption | Whole module tree reinstalled; `build/deploy-kernel.sh` now always ships boot_a + the complete module tree together. Shutdown: 5 min → ~1 s |
| ccmni | Stale MDT tag (`0x46464646`) in a reused skb head → `memcpy` past the tail on TX; only bites with IPsec ESP (no SG, zero tailroom) | Bounded append (commit 3ce28cc) |
| ccmni | `ndo_siocdevprivate` was `ccmni_ioctl` cast to a different signature: the user pointer landed in `cmd` (private ioctls dead; kCFI trap if enabled) | Proper wrapper |
| DPMAIF RX | BAT count reported to HW at PD time although the refill happened later and could fail → HW could DMA into a slot whose skb was already in the stack; a full skb list or failed refill made the next pass unmap/push the same skb twice or assert the modem | Room in the skb list is checked before a packet is consumed; once handed to the stack the PIT entry always advances; only really refilled BAT slots are reported; shortfall kept in `bat_deficit` and topped up from the blocking work |
| port_net | Packets for a down ccmni queued without bound (IMS PDN before q25-imsd starts) | Cap at 512, drop beyond |
| ccmni / port_net | Modem not READY (reset) mapped to TX_BUSY without stopping the queue → tight retry loop; each retry appended another MDT tag; ccmni-lan busy path set `skb->len` without the tail | Dropped like MD_NO_READY; tag trimmed on busy; `skb_trim` |
| pinctrl (0030) | `mt6789_pull_type[]` 188 entries for 222 pins → OOB read on bias get/set of GPIO188–221 | Sized to the pin table |
| synaptics_dsx | Chip-reported query sizes used as read length into fixed stack buffers; `num_of_intr_regs` uncapped | Clamped |
| upm6722 | `devm_power_supply_register` + manual unregister (double free on remove/probe error) | Manual unregisters removed |
| q20_keyboard | Firmware-upgrade proc file 0777 | 0600 |
| q25-imsd | LAN devices could inject SMS: sockets on the global IMS address accepted any source | Only the P-CSCF is accepted; requests only on ESP-protected ports |
| q25-imsd | IK on the `ip xfrm` command line (visible in /proc, logged on error) | `ip -batch -` on stdin, keys redacted from errors |
| q25-imsd | Flushed **all** system xfrm state at start | Removes only its own SAs (recorded in /run) |
| q25-imsd | Reader thread could die silently (bad hex) → daemon alive but deaf | Exits for a clean restart; pending-dict race fixed |
| q25-imsd | Hardcoded user `max`; sms.jsonl world-readable and unbounded; concat parts never expire | Desktop user lookup; 0600 + rotation; parts expire after 24 h |
| q25-imsd | No deregistration (stale contacts for up to 1 h after each restart) | SIGTERM → REGISTER Expires: 0 (answers the S-CSCF re-challenge), verified 200 OK |
| q25-imsd | Security-Server parameters merged across offers, alg/ealg unchecked | Per-offer parsing, supported offer chosen, all echoed in Security-Verify |
| q25-imsd | Wrote the ims APN while the plugin did the same (CME 5843 race) | Left to the plugin |
| MM bridge | Blocking writes on ModemManager's main loop: a stalled client froze MM | Non-blocking; stalled client is dropped |
| MM bridge | Disposed modem could unlink the socket of its successor | Unlinks only its own inode |
| Build | Default password = user name with sshd password auth on | Key-only sshd drop-in when `SSH_KEY` is given; warning otherwise |
| Build | `pacman -U` of `out/packages` failed on duplicate targets when a package had been rebuilt | Newest build of each package only |
| Build | NM keyfile shipped 0644 (ignored by NM); `pkill -f gpg-agent` killed host agents; `Q25_APN` lost through sudo; `__pycache__` copied into the image | Fixed |
| Config | No lockup/hung-task detection, no KFENCE, no slab/list hardening, `NR_CPUS=512` | Soft/hard (buddy) lockup, hung task, WQ watchdog (all report-only), KFENCE, freelist + list hardening, `NR_CPUS=8` |

## SMS robustness (verified on the network)

Re-registration as a transaction (60 s test interval, several cycles, SA count
constant at 4), P-CSCF failover (primary blocked with nftables → registered via
the backup in 2 s), bearer loss / address change reaction, SIP over TCP,
AKA resync path, clean deregistration, spoofed packets dropped. Suspend is
disabled system-wide (`AllowSuspend=no`), so no suspend handling yet.

## Open — needs a decision or a careful session

Kernel (from `0024`, earlier agent):
1. **head.S ignores the bootloader DTB** and pokes two physical addresses
   before the MMU is up. Kernel and LK DTB disagree on memory size,
   reservations, kaslr/rng seed. Moving to the LK-passed DTB (or an overlay)
   is a boot-chain change: do it with the phone on the desk.
2. ~~Generic arm64 code patched~~ **fixed (0032, kernel #49)**: printk -> raw
   pmsg copy hook, its 16 fixmap slots, the head.S "Q250OK!" marker and the
   TOPRGU arming through `FIX_TEXT_POKE0` are gone; ramoops + mtk_wdt cover
   them. (A slot-b test first looked like a boot loop: LK does not boot slot
   b after `fastboot set_active b` here, so test kernels go to boot_a, with
   power + volume-up -> fastboot as recovery.)
3. genpd core special-cases two out-of-tree compatibles (`power_on=false`);
   should be `dev_pm_domain_attach_by_id()` in those drivers.
4. DISP domain always-on without taking its clocks; works only thanks to
   `clk_ignore_unused` + `pd_ignore_unused` + `CLK_IGNORE_UNUSED` on all MM
   gates. Multimedia clock controllers (img/ipe/vdec/venc/cam, mfgcfg) have no
   `power-domains` → reading them with the domain off can hang the bus.
   Measured on kernel #49: 234 of 365 clocks are on in hardware but claimed by
   no driver (cam 19, afe 18, mdp 18, mm 18, univpll 17, ifrao 14, mainpll
   14, apll*, ipe, aud, ...); they only stay on because of the ignore-unused
   flags. Plan, one step per session, each tested on boot_a:
   (a) measure battery drain at idle/screen-off to size the gain;
   (b) display path: DISP domain clocks as consumers, DSI PHY balance (item 5);
   (c) multimedia/camera blocks (mdp/img/ipe/cam + power-domains);
   (d) audio, modem (md/ifrao), Wi-Fi (conn) last;
   then drop clk_ignore_unused / pd_ignore_unused.
5. DSI handoff path skips `phy_power_on` but poweroff calls `phy_power_off`
   (count goes negative); mmsys routes through the PQ chain while the declared
   path is OVL0→RDMA0→DSI0; several shared DRM/mutex changes apply to every
   SoC; `mfg_pll_sel` got mfg_ref_sel's update bit.
6. auxadc `PROCESSED` returns m°C on voltage channels (ABI); arch timer flag
   LEVEL_LOW vs stock LEVEL_HIGH.

Drivers:
- oca72xxx firmware parsers trust file offsets/counts (needs a crafted file);
  `ocarw` leaks `reg_lock` on I2C failure.
- synaptics: DRM panel follower outlives `rmi4_data` on unbind; irq_work
  not cancelled.
- Wi-Fi: remain-on-channel/mgmt-tx cookie mismatch with 7.3 cfg80211 (breaks
  GAS/ANQP/P2P); GRO list flush removed.
- ccmni busy-loops while the modem resets (`-ENODEV` → TX_BUSY without
  stopping the queue).
- Remove/unbind lifetime bugs (only on rmmod): upm6922, st21nfc, aw36518,
  oca72xxx, q25_afe_pcm.
- bbqX0kbd proc toggles are 0666 (left: userspace may rely on them).

Fresh image: was blocked by Arch Linux ARM moving phosh/phoc to 0.59.0 while
the Q25 packages pinned 0.57.0. Resolved 2026-11-22: stock ALARM phoc 0.59 embeds
wlroots without the layer-shell 0-height check, so `packages/phoc-q25` is gone;
`phosh-q25-keyboard` rebuilt for 0.59.0. Phone upgraded (133 packages incl.
systemd 262, Mesa 26.3.1); the previous agent's unused oFono/gbinder packages
were removed because `libgbinder-radio` blocked the upgrade.

Userspace:
- Transmit guards (D-Bus/polkit rules from earlier experiments, only ever on
  this phone): removed 2026-11-22 (backup in `/root/q25-backup-20261121/`).
  Without VoLTE an outgoing call cannot reach the network anyway.
- `webjetd` on the phone is the owner's personal VPN: intentionally not part
  of the image. Old smoke-test binaries in `/usr/local` are not either.
- `mm-ims-observer-mtk-ccci.c` (+ test) is superseded by q25-imsd.

## Touch smoothness (measured 2026-11-22)

S2716B reports at ~110 Hz (9 ms, steady) over a 400 kHz I2C bus it has to
itself. During continuous drags no reports are dropped (0.1 % of intervals
>20 ms, none >40 ms) and USB charging makes no difference. Path noise is
~0.4 px rms still, 0.7-1.6 px rms moving (Y about twice X). The visible shake
when dragging content is mostly 110 Hz input sampled by a 60 Hz compositor with
no touch resampling (1 or 2 reports per frame). The driver's optional One Euro
filter (`one_euro_enable`) reduces noise but users feel the lag; left off.

## GPU

Mali-G57 MC2 is Valhall **v9, job-manager**. Panfrost GLES 3.1 / GL 3.1 is
the only driver and it performs well: glmark2-es2-wayland 720×720 = **916**
(terrain 36 fps, refract 80, desktop blur 182), DVFS scales 390→1100 MHz,
~36 °C peak, no faults. **No Vulkan path exists**: PanVK supports v6/7 and
v10+, not v9 (hard "Mali-G57 not supported", even with
`PAN_I_WANT_A_BROKEN_VULKAN_DRIVER`), so no Zink either; Panthor is CSF-only.
Worthwhile: keep Mesa current; `setcap cap_sys_nice+ep` on phoc for a
high-priority GPU context; optionally trim the 36-point OPP table. No hardware
video decode on mainline for MT6789 (vendor-only VCU protocol).
