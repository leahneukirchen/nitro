# Simplest Possible Adaptable Transfer

SPAT is a simple binary protocol.  Packets consist of a three byte
header and flexible payload.

	+--------------+---------------+---------+=============+
	| LOW_BYTE_LEN | HIGH_BYTE_LEN | COMMAND | PAYLOAD ... |
	+--------------+---------------+---------+=============+

Where the length of the PAYLOAD only is stored in the first two bytes
as little endian 16-bit integer.  Packets longer than 65534 cannot be
sent over SPAT and must be reassembled on the application level.

The actual PAYLOAD depends on the COMMAND being used.  Customarily
SPAT uses little endian byte order.

Additionally, two special lengths are used to frame inner packets by
enclosing them with a COMMAND:

	+------+------+---------+===================+------+------+---------+
	| 0xFF | 0xFF | COMMAND | INNER PACKETS ... | 0xFE | 0xFF | COMMAND |
	+------+------+---------+===================+------+------+---------+

It is not permitted to nest packets of the same COMMAND type.

This way, unknown packets and unknown frames can be skipped without recursion.

# Usage in nitro

The commands nitro uses are listed in <nitro.h> as "enum tags".  Old
tag ids must not be reused for backwards compatibility.

## License

SPDX-License-Identifier: 0BSD
