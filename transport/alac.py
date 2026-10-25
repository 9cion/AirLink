"""AirLink's verbatim ALAC packet serializer (stereo, signed 16-bit).

The field layout follows the ALAC channel-pair element format: element tag,
instance, reserved bits, optional frame count, sample data and end element.
Samples are preserved exactly; no prediction or lossy processing is involved.
Format reference: https://github.com/macosforge/alac
"""


def encode_packet(stereo_be: bytes) -> bytes:
    frames, remainder = divmod(len(stereo_be), 4)
    if remainder or not 1 <= frames <= 352:
        raise ValueError("ALAC input must contain 1..352 complete stereo frames")
    explicit_count = int(frames != 352)
    fields = [(1, 3), (0, 4), (0, 12), (explicit_count, 1), (0, 2), (1, 1)]
    if explicit_count:
        fields.append((frames, 32))
    bits = width = 0
    for value, length in fields:
        bits = (bits << length) | value
        width += length
    sample_bits = len(stereo_be) * 8
    bits = (bits << sample_bits) | int.from_bytes(stereo_be, "big")
    bits = (bits << 3) | 7  # End element.
    width += sample_bits + 3
    alignment = -width % 8
    return (bits << alignment).to_bytes((width + alignment) // 8, "big")
