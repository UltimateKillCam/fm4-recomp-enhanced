using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

// Modified 2026-10-18. Deterministic stored ZIP writer for PS5.1 and PS7.
// GPL-3.0-only; part of the FM4 enhanced derivative.
namespace Fm4.Release
{
public static class SourceArchive
{
    private const uint LocalSignature = 0x04034b50;
    private const uint CentralSignature = 0x02014b50;
    private const uint EndSignature = 0x06054b50;
    private const ushort Utf8Flag = 0x0800;
    private const ushort DosDate = 0x2821; // 2000-01-01
    private static readonly uint[] CrcTable = BuildCrcTable();

    private sealed class Entry
    {
        public string FullPath;
        public string Name;
        public byte[] NameBytes;
        public uint Crc;
        public uint Size;
        public uint LocalOffset;
    }

    public static void Create(string sourceRoot, string archivePath)
    {
        string root = Path.GetFullPath(sourceRoot).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        string zip = Path.GetFullPath(archivePath);
        List<Entry> entries = ReadEntries(root);
        if (entries.Count >= UInt16.MaxValue) throw new InvalidDataException("ZIP entry count exceeds the classic ZIP limit.");
        foreach (Entry entry in entries)
        {
            entry.Crc = ComputeFileCrc(entry.FullPath, out entry.Size);
            if (entry.NameBytes.Length > UInt16.MaxValue) throw new InvalidDataException("ZIP entry name is too long: " + entry.Name);
        }

        bool created = false;
        try
        {
            using (FileStream stream = new FileStream(zip, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None))
            using (BinaryWriter writer = new BinaryWriter(stream, Encoding.UTF8))
            {
                created = true;
                foreach (Entry entry in entries)
                {
                    long offset = stream.Position;
                    if (offset > UInt32.MaxValue) throw new InvalidDataException("ZIP exceeds the classic ZIP offset limit.");
                    entry.LocalOffset = (uint)offset;
                    WriteLocalHeader(writer, entry);
                    writer.Write(entry.NameBytes);
                    uint secondSize;
                    uint secondCrc = CopyFileAndComputeCrc(entry.FullPath, writer, out secondSize);
                    if (secondCrc != entry.Crc || secondSize != entry.Size)
                        throw new IOException("Source changed while creating the archive: " + entry.Name);
                }

                long centralOffset = stream.Position;
                if (centralOffset > UInt32.MaxValue) throw new InvalidDataException("ZIP exceeds the classic ZIP central-directory offset limit.");
                foreach (Entry entry in entries) WriteCentralHeader(writer, entry);
                long centralSize = stream.Position - centralOffset;
                if (centralSize > UInt32.MaxValue) throw new InvalidDataException("ZIP central directory exceeds the classic ZIP limit.");
                WriteEndRecord(writer, (ushort)entries.Count, (uint)centralSize, (uint)centralOffset);
                writer.Flush();
            }
        }
        catch
        {
            if (created && File.Exists(zip))
            {
                try { File.Delete(zip); } catch { }
            }
            throw;
        }
    }

    private static List<Entry> ReadEntries(string root)
    {
        string prefix = root + Path.DirectorySeparatorChar;
        string[] paths = Directory.GetFiles(root, "*", SearchOption.AllDirectories);
        List<Entry> entries = new List<Entry>(paths.Length);
        foreach (string path in paths)
        {
            string full = Path.GetFullPath(path);
            if (!full.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Source file escaped archive root: " + full);
            string name = full.Substring(prefix.Length).Replace(Path.DirectorySeparatorChar, '/');
            if (Path.AltDirectorySeparatorChar != Path.DirectorySeparatorChar)
                name = name.Replace(Path.AltDirectorySeparatorChar, '/');
            entries.Add(new Entry { FullPath = full, Name = name, NameBytes = Encoding.UTF8.GetBytes(name) });
        }
        entries.Sort(delegate(Entry left, Entry right) { return StringComparer.Ordinal.Compare(left.Name, right.Name); });
        return entries;
    }

    private static void WriteLocalHeader(BinaryWriter writer, Entry entry)
    {
        writer.Write(LocalSignature);
        writer.Write((ushort)20);
        writer.Write(Utf8Flag);
        writer.Write((ushort)0); // stored, no compression
        writer.Write((ushort)0); // midnight
        writer.Write(DosDate);
        writer.Write(entry.Crc);
        writer.Write(entry.Size);
        writer.Write(entry.Size);
        writer.Write((ushort)entry.NameBytes.Length);
        writer.Write((ushort)0); // no extra field
    }

    private static void WriteCentralHeader(BinaryWriter writer, Entry entry)
    {
        writer.Write(CentralSignature);
        writer.Write((ushort)20); // made by DOS/FAT, version 2.0
        writer.Write((ushort)20);
        writer.Write(Utf8Flag);
        writer.Write((ushort)0);
        writer.Write((ushort)0);
        writer.Write(DosDate);
        writer.Write(entry.Crc);
        writer.Write(entry.Size);
        writer.Write(entry.Size);
        writer.Write((ushort)entry.NameBytes.Length);
        writer.Write((ushort)0); // extra length
        writer.Write((ushort)0); // comment length
        writer.Write((ushort)0); // disk number
        writer.Write((ushort)0); // internal attributes
        writer.Write((uint)0); // external attributes
        writer.Write(entry.LocalOffset);
        writer.Write(entry.NameBytes);
    }

    private static void WriteEndRecord(BinaryWriter writer, ushort count, uint directorySize, uint directoryOffset)
    {
        writer.Write(EndSignature);
        writer.Write((ushort)0);
        writer.Write((ushort)0);
        writer.Write(count);
        writer.Write(count);
        writer.Write(directorySize);
        writer.Write(directoryOffset);
        writer.Write((ushort)0);
    }

    private static uint ComputeFileCrc(string path, out uint size)
    {
        uint crc = 0xffffffff;
        long total = 0;
        byte[] buffer = new byte[65536];
        using (FileStream input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            int read;
            while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
            {
                total += read;
                if (total > UInt32.MaxValue) throw new InvalidDataException("ZIP entry exceeds the classic ZIP size limit: " + path);
                crc = UpdateCrc(crc, buffer, read);
            }
        }
        size = (uint)total;
        return crc ^ 0xffffffff;
    }

    private static uint CopyFileAndComputeCrc(string path, BinaryWriter writer, out uint size)
    {
        uint crc = 0xffffffff;
        long total = 0;
        byte[] buffer = new byte[65536];
        using (FileStream input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            int read;
            while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
            {
                total += read;
                if (total > UInt32.MaxValue) throw new InvalidDataException("ZIP entry exceeds the classic ZIP size limit: " + path);
                writer.Write(buffer, 0, read);
                crc = UpdateCrc(crc, buffer, read);
            }
        }
        size = (uint)total;
        return crc ^ 0xffffffff;
    }

    private static uint UpdateCrc(uint crc, byte[] buffer, int count)
    {
        for (int i = 0; i < count; i++) crc = CrcTable[(crc ^ buffer[i]) & 0xff] ^ (crc >> 8);
        return crc;
    }

    private static uint[] BuildCrcTable()
    {
        uint[] table = new uint[256];
        for (uint i = 0; i < 256; i++)
        {
            uint value = i;
            for (int bit = 0; bit < 8; bit++) value = (value & 1) != 0 ? 0xedb88320 ^ (value >> 1) : value >> 1;
            table[i] = value;
        }
        return table;
    }
}
}
