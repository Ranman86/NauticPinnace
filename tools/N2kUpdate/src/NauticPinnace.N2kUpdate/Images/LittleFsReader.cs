using System.Buffers.Binary;
using System.Text;

namespace NauticPinnace.N2kUpdate.Images;

/// <summary>
/// A minimal read-only LittleFS v2 reader, just enough to take one file out of the
/// root directory of a <c>littlefs.bin</c> as mklittlefs builds it for a release.
/// Release ZIPs up to v1.2.0 carry the web UI only inside littlefs.bin; this lets the
/// updater use such a folder without a repository checkout.
///
/// Format (littlefs SPEC.md): every directory is a metadata pair; a metadata block
/// starts with a 32-bit revision count followed by commits of 32-bit big-endian
/// tags, each XOR-ed with the previous one ([valid:1][type:11][id:10][size:10]),
/// closed by a CRC tag. Files are inline in the tag or a CTZ skip-list of blocks.
/// </summary>
public sealed class LittleFsReader
{
    private const int TypeReg = 0x001, TypeSuperblock = 0x0FF;
    private const int TypeInline = 0x201, TypeCtz = 0x202;
    private const int TypeCreate = 0x401, TypeDelete = 0x4FF;
    private const int TypeHardTail = 0x601;

    private readonly byte[] _img;
    private readonly int _blockSize;

    private LittleFsReader(byte[] image, int blockSize)
    {
        _img = image;
        _blockSize = blockSize;
    }

    public int BlockSize => _blockSize;

    /// <summary>Opens an image; tries the usual block sizes and checks the superblock.</summary>
    public static LittleFsReader? TryOpen(byte[] image)
    {
        foreach (int bs in new[] { 4096, 512, 1024, 2048, 8192, 16384 })
        {
            if (image.Length < 2 * bs) continue;
            var r = new LittleFsReader(image, bs);
            try
            {
                var entries = r.ReadDirectory(0, 1);
                if (entries.Any(e => e.Type == TypeSuperblock && e.Name == "littlefs")) return r;
            }
            catch (InvalidDataException) { }
            catch (ArgumentOutOfRangeException) { }
        }
        return null;
    }

    /// <summary>Reads a file from the root directory, or null if it is not there.</summary>
    public byte[]? ReadRootFile(string name)
    {
        var e = ReadDirectory(0, 1).FirstOrDefault(x => x.Type == TypeReg && x.Name == name);
        if (e is null || e.Struct is null) return null;
        if (e.StructType == TypeInline) return e.Struct;
        if (e.StructType == TypeCtz && e.Struct.Length >= 8)
        {
            uint head = BinaryPrimitives.ReadUInt32LittleEndian(e.Struct);
            uint size = BinaryPrimitives.ReadUInt32LittleEndian(e.Struct.AsSpan(4));
            return ReadCtz(head, size);
        }
        return null;
    }

    public IEnumerable<string> RootFileNames() =>
        ReadDirectory(0, 1).Where(e => e.Type == TypeReg).Select(e => e.Name);

    private sealed class Entry
    {
        public int Type = -1;
        public string Name = "";
        public int StructType = -1;
        public byte[]? Struct;
    }

    private List<Entry> ReadDirectory(uint a, uint b)
    {
        var all = new List<Entry>();
        var seen = new HashSet<uint>();
        uint pa = a, pb = b;
        while (true)
        {
            if (!seen.Add(pa)) throw new InvalidDataException("metadata tail loop");
            var (entries, tail) = FetchPair(pa, pb);
            all.AddRange(entries.Where(e => e.Type >= 0));
            if (tail is null) break;
            (pa, pb) = tail.Value;
        }
        return all;
    }

    private (List<Entry> Entries, (uint, uint)? HardTail) FetchPair(uint a, uint b)
    {
        // The block with the newer revision count holds the current state; fall back
        // to the other one if it has no valid commit.
        uint revA = BinaryPrimitives.ReadUInt32LittleEndian(Block(a)), revB = BinaryPrimitives.ReadUInt32LittleEndian(Block(b));
        uint first = (int)(revA - revB) >= 0 ? a : b, second = first == a ? b : a;
        var r = FetchBlock(first);
        if (r is null) r = FetchBlock(second);
        if (r is null) throw new InvalidDataException("no valid metadata commit");
        return r.Value;
    }

    private (List<Entry>, (uint, uint)?)? FetchBlock(uint block)
    {
        var blk = Block(block);
        var entries = new List<Entry>();
        var pending = new List<Entry>();      // state of the commit in progress
        (uint, uint)? tail = null, pendingTail = null;
        bool anyCommit = false;
        uint ptag = 0xFFFFFFFF;
        uint crc = 0xFFFFFFFF;
        crc = LfsCrc(crc, blk[..4]);
        int off = 0;
        int dsize = 4;
        CopyEntries(entries, pending);
        while (true)
        {
            off += dsize;
            if (off + 4 > _blockSize) break;
            uint raw = BinaryPrimitives.ReadUInt32BigEndian(blk[off..]);
            crc = LfsCrc(crc, blk.Slice(off, 4));
            uint tag = raw ^ ptag;
            if ((tag & 0x80000000) != 0) break;              // not programmed / invalid
            int size = (int)(tag & 0x3FF);
            dsize = 4 + (size == 0x3FF ? 0 : size);
            if (off + dsize > _blockSize) break;
            int type = (int)((tag >> 20) & 0x7FF);
            int id = (int)((tag >> 10) & 0x3FF);
            ptag = tag;

            if ((type & 0x780) == 0x500)
            {
                // CRC tag: the commit is valid if the stored CRC matches.
                uint stored = BinaryPrimitives.ReadUInt32LittleEndian(blk[(off + 4)..]);
                if (crc != stored) break;
                ptag ^= (uint)((type & 0xFF) & 1) << 31;
                CopyEntries(entries, pending);
                tail = pendingTail;
                anyCommit = true;
                crc = 0xFFFFFFFF;
                continue;
            }
            crc = LfsCrc(crc, blk.Slice(off + 4, dsize - 4));
            var data = blk.Slice(off + 4, dsize - 4);

            if (type == TypeCreate)
            {
                pending.Insert(Math.Min(id, pending.Count), new Entry());
            }
            else if (type == TypeDelete)
            {
                if (id < pending.Count) pending.RemoveAt(id);
            }
            else if ((type & 0x700) == 0x000)
            {
                Grow(pending, id);
                pending[id].Type = type;
                pending[id].Name = Encoding.UTF8.GetString(data);
            }
            else if ((type & 0x700) == 0x200)
            {
                Grow(pending, id);
                pending[id].StructType = type;
                pending[id].Struct = data.ToArray();
            }
            else if (type == TypeHardTail && data.Length >= 8)
            {
                pendingTail = (BinaryPrimitives.ReadUInt32LittleEndian(data), BinaryPrimitives.ReadUInt32LittleEndian(data[4..]));
            }
        }
        return anyCommit ? (entries, tail) : null;
    }

    private static void Grow(List<Entry> list, int id)
    {
        while (list.Count <= id) list.Add(new Entry());
    }

    private static void CopyEntries(List<Entry> committed, List<Entry> pending)
    {
        committed.Clear();
        foreach (var e in pending)
            committed.Add(new Entry { Type = e.Type, Name = e.Name, StructType = e.StructType, Struct = e.Struct });
    }

    private byte[] ReadCtz(uint head, uint size)
    {
        if (size == 0) return [];
        // Index of the block holding the last byte (lfs_ctz_index), then walk back.
        long last = CtzIndex(size - 1);
        var blocks = new uint[last + 1];
        uint cur = head;
        for (long i = last; i >= 0; i--)
        {
            blocks[i] = cur;
            if (i > 0) cur = BinaryPrimitives.ReadUInt32LittleEndian(Block(cur));
        }
        var outp = new byte[size];
        int pos = 0;
        for (long i = 0; i <= last && pos < size; i++)
        {
            int skip = i == 0 ? 0 : 4 * (System.Numerics.BitOperations.TrailingZeroCount((ulong)i) + 1);
            int n = Math.Min(_blockSize - skip, (int)size - pos);
            Block(blocks[i]).Slice(skip, n).CopyTo(outp.AsSpan(pos));
            pos += n;
        }
        return outp;
    }

    private long CtzIndex(long off)
    {
        long b = _blockSize - 2 * 4;
        long i = off / b;
        if (i == 0) return 0;
        i = (off - 4 * (System.Numerics.BitOperations.PopCount((ulong)(i - 1)) + 2)) / b;
        return i;
    }

    private ReadOnlySpan<byte> Block(uint n)
    {
        long start = (long)n * _blockSize;
        if (start < 0 || start + _blockSize > _img.Length) throw new InvalidDataException($"block {n} outside the image");
        return _img.AsSpan((int)start, _blockSize);
    }

    /// <summary>littlefs's CRC-32: reflected 0x04C11DB7, no final inversion.</summary>
    private static uint LfsCrc(uint crc, ReadOnlySpan<byte> data) => ~Protocol.Crc32.Update(~crc, data);
}
