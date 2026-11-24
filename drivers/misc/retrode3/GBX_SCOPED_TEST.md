# Scoped GBx transport (hardware test required)

The gbx_md_test parameter remains compatible with the CLI hardware profile.
While enabled, regular slot IO, Flash ioctls, controller polling and raw
GPIO access are excluded. GBx FDs prevent profile transitions and voltage
changes until closed. The common CLI must still hold its process-wide
bus lock for the complete probe or dump. The kernel serializes each file
operation and profile change. On disable, release data and restore native
address directions with selects inactive, then clear gbx_active. Partial
restore errors retain the gate and may be retried.

The proven bench pin mapping and GBA random/burst strobes are unchanged.
This is not electrical certification: the passive GBx adapter may still
be physically attached when native address outputs are restored. Test
with backed-up cartridges and one cartridge at a time. In particular
verify GB audio/GBA IRQ and /WR idle behavior on hardware before release.

Build only the test module. Test GBA header and full CRC, normal MD/SMS/SNES/NES
after removing the adapter, failed probe recovery, cancellation and controller
polling. Never overwrite the installed module until verified.
