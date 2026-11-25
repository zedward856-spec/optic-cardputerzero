"""A small QR code encoder (byte mode, versions 1-10, ECC level L/M/Q/H), after Project Nayuki's
reference design.  python3 qr.py TEXT [L|M|Q|H]  ->  prints the module matrix as lines of 0/1.
(H survives ~30% of the code being covered: room for a logo in the middle.)"""
import sys

# per ECC level: codewords per block and number of blocks, versions 1-10 (index 0 unused)
ECC_TABLE = {
    'L': ([-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18], [-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4]),
    'M': ([-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26], [-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5]),
    'Q': ([-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24], [-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8]),
    'H': ([-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28], [-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8]),
}
FORMAT_BITS = {'L': 1, 'M': 0, 'Q': 3, 'H': 2}
ECC_CW, BLOCKS = ECC_TABLE['M']
LEVEL = 'M'


def raw_modules(v):
    r = (16 * v + 128) * v + 64
    if v >= 2:
        n = v // 7 + 2
        r -= (25 * n - 10) * n - 55
        if v >= 7:
            r -= 36
    return r


def data_cw(v):
    return raw_modules(v) // 8 - ECC_CW[v] * BLOCKS[v]


def gf_mul(x, y):
    z = 0
    for i in range(7, -1, -1):
        z = (z << 1) ^ ((z >> 7) * 0x11D)
        z ^= ((y >> i) & 1) * x
    return z & 0xFF


def rs_divisor(deg):
    r = [0] * (deg - 1) + [1]
    root = 1
    for _ in range(deg):
        for j in range(deg):
            r[j] = gf_mul(r[j], root)
            if j + 1 < deg:
                r[j] ^= r[j + 1]
        root = gf_mul(root, 2)
    return r


def rs_remainder(data, div):
    r = [0] * len(div)
    for b in data:
        f = b ^ r.pop(0)
        r.append(0)
        for i, c in enumerate(div):
            r[i] ^= gf_mul(c, f)
    return r


def align_positions(v):
    if v == 1:
        return []
    n = v // 7 + 2
    step = 26 if v == 32 else (v * 4 + n * 2 + 1) // (n * 2 - 2) * 2
    res = [6]
    for i in range(n - 1):
        res.insert(1, v * 4 + 10 - i * step)
    return res


def encode(text, level='M'):
    global ECC_CW, BLOCKS, LEVEL
    ECC_CW, BLOCKS = ECC_TABLE[level]
    LEVEL = level
    data = text.encode('utf-8')
    for v in range(1, 11):
        cap = data_cw(v) * 8
        need = 4 + (8 if v <= 9 else 16) + 8 * len(data)
        if need <= cap:
            break
    else:
        raise ValueError('too long')
    bits = []
    def put(val, n):
        for i in range(n - 1, -1, -1):
            bits.append((val >> i) & 1)
    put(4, 4)
    put(len(data), 8 if v <= 9 else 16)
    for b in data:
        put(b, 8)
    cap = data_cw(v) * 8
    put(0, min(4, cap - len(bits)))
    put(0, (8 - len(bits) % 8) % 8)
    pad = 0xEC
    while len(bits) < cap:
        put(pad, 8)
        pad ^= 0xEC ^ 0x11
    words = [int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)]

    # split into blocks, add ECC, interleave
    nb = BLOCKS[v]
    ecl = ECC_CW[v]
    raw = raw_modules(v) // 8
    short = nb - raw % nb
    short_len = raw // nb
    div = rs_divisor(ecl)
    blocks = []
    k = 0
    for i in range(nb):
        n = short_len - ecl + (0 if i < short else 1)
        dat = words[k:k + n]
        k += n
        ecc = rs_remainder(dat, div)
        if i < short:
            dat = dat + [0]
        blocks.append(dat + ecc)
    out = []
    for i in range(len(blocks[0])):
        for j, b in enumerate(blocks):
            if i != short_len - ecl or j >= short:
                out.append(b[i])

    size = v * 4 + 17
    mod = [[0] * size for _ in range(size)]
    fn = [[False] * size for _ in range(size)]
    def setf(x, y, d):
        mod[y][x] = 1 if d else 0
        fn[y][x] = True

    # timing
    for i in range(size):
        setf(6, i, i % 2 == 0)
        setf(i, 6, i % 2 == 0)
    # finders
    for cx, cy in ((3, 3), (size - 4, 3), (3, size - 4)):
        for dy in range(-4, 5):
            for dx in range(-4, 5):
                d = max(abs(dx), abs(dy))
                x, y = cx + dx, cy + dy
                if 0 <= x < size and 0 <= y < size:
                    setf(x, y, d not in (2, 4))
    # alignment
    al = align_positions(v)
    na = len(al)
    for i in range(na):
        for j in range(na):
            if (i == 0 and j == 0) or (i == 0 and j == na - 1) or (i == na - 1 and j == 0):
                continue
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    setf(al[i] + dx, al[j] + dy, max(abs(dx), abs(dy)) != 1)
    # format (dummy, drawn for real later) + version
    def draw_format(mask):
        data5 = (FORMAT_BITS[LEVEL] << 3) | mask
        rem = data5
        for _ in range(10):
            rem = (rem << 1) ^ ((rem >> 9) * 0x537)
        b = ((data5 << 10) | rem) ^ 0x5412
        for i in range(0, 6):
            setf(8, i, (b >> i) & 1)
        setf(8, 7, (b >> 6) & 1)
        setf(8, 8, (b >> 7) & 1)
        setf(7, 8, (b >> 8) & 1)
        for i in range(9, 15):
            setf(14 - i, 8, (b >> i) & 1)
        for i in range(0, 8):
            setf(size - 1 - i, 8, (b >> i) & 1)
        for i in range(8, 15):
            setf(8, size - 15 + i, (b >> i) & 1)
        setf(8, size - 8, 1)
    draw_format(0)
    if v >= 7:
        rem = v
        for _ in range(12):
            rem = (rem << 1) ^ ((rem >> 11) * 0x1F25)
        b = (v << 12) | rem
        for i in range(18):
            bit = (b >> i) & 1
            a, c = size - 11 + i % 3, i // 3
            setf(a, c, bit)
            setf(c, a, bit)

    # data, zigzag
    i = 0
    right = size - 1
    while right >= 1:
        if right == 6:
            right = 5
        for vert in range(size):
            for j in range(2):
                x = right - j
                up = ((right + 1) & 2) == 0
                y = size - 1 - vert if up else vert
                if not fn[y][x] and i < len(out) * 8:
                    mod[y][x] = (out[i >> 3] >> (7 - (i & 7))) & 1
                    i += 1
        right -= 2

    masks = [
        lambda x, y: (x + y) % 2 == 0, lambda x, y: y % 2 == 0, lambda x, y: x % 3 == 0,
        lambda x, y: (x + y) % 3 == 0, lambda x, y: (x // 3 + y // 2) % 2 == 0,
        lambda x, y: x * y % 2 + x * y % 3 == 0, lambda x, y: (x * y % 2 + x * y % 3) % 2 == 0,
        lambda x, y: ((x + y) % 2 + x * y % 3) % 2 == 0]

    def apply(m):
        for y in range(size):
            for x in range(size):
                if not fn[y][x] and masks[m](x, y):
                    mod[y][x] ^= 1

    def penalty():
        p = 0
        for grid in (mod, [list(c) for c in zip(*mod)]):
            for row in grid:
                run, col = 1, row[0]
                for k in range(1, size):
                    if row[k] == col:
                        run += 1
                    else:
                        if run >= 5:
                            p += run - 2
                        run, col = 1, row[k]
                if run >= 5:
                    p += run - 2
                s = ''.join(map(str, row))
                p += 40 * (s.count('10111010000') + s.count('00001011101'))
        for y in range(size - 1):
            for x in range(size - 1):
                c = mod[y][x]
                if c == mod[y][x + 1] == mod[y + 1][x] == mod[y + 1][x + 1]:
                    p += 3
        dark = sum(map(sum, mod))
        total = size * size
        k = (abs(dark * 20 - total * 10) + total - 1) // total - 1
        p += k * 10
        return p

    best, best_p = 0, None
    for m in range(8):
        apply(m)
        draw_format(m)
        pp = penalty()
        if best_p is None or pp < best_p:
            best, best_p = m, pp
        apply(m)
    apply(best)
    draw_format(best)
    return mod


if __name__ == '__main__':
    for row in encode(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else 'M'):
        print(''.join(map(str, row)))
