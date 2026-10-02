#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <juce_audio_formats/juce_audio_formats.h>

namespace soundsplice::engine
{
/**
    What an exported file says about itself, and its chapters.

    Each format has its own way, and each is written the way it's read:
      - WAV:  a LIST/INFO chunk (title, artist, album, ...) and a BWF bext
              chunk (description, originator, date), through JUCE's writer.
      - Ogg:  Vorbis comments, through JUCE's writer.
      - FLAC: a VORBIS_COMMENT block, with chapters as CHAPTER001= /
              CHAPTER001NAME= comments, and a PICTURE block for the cover,
              put into the file after JUCE has written it (its writer has
              no metadata).
      - MP3:  an ID3v2.4 tag in front of the audio: text frames, APIC for
              the cover, and CHAP and CTOC frames for chapters (the podcast
              chapter format).
    AIFF carries none of it.
*/
struct ExportChapter
{
    double       startSeconds = 0.0;
    double       endSeconds   = 0.0;
    juce::String title;
};

struct ExportTags
{
    juce::String title, artist, album, year, genre, comment, track;
    juce::File   coverArt;
    std::vector<ExportChapter> chapters;

    bool empty() const
    {
        return title.isEmpty() && artist.isEmpty() && album.isEmpty() && year.isEmpty() && genre.isEmpty()
            && comment.isEmpty() && track.isEmpty() && ! coverArt.existsAsFile() && chapters.empty();
    }
};

namespace tags
{
    /** The metadata JUCE's writer takes, for the formats whose writer takes it. */
    inline std::unordered_map<juce::String, juce::String> writerMetadata(const ExportTags& t, bool wav, bool ogg)
    {
        std::unordered_map<juce::String, juce::String> m;
        const auto put = [&m](const char* key, const juce::String& value)
        {
            if (value.isNotEmpty())
                m[key] = value;
        };
        if (wav)
        {
            put(juce::WavAudioFormat::riffInfoTitle, t.title);
            put(juce::WavAudioFormat::riffInfoArtist, t.artist);
            put(juce::WavAudioFormat::riffInfoProductName, t.album);
            put(juce::WavAudioFormat::riffInfoDateCreated, t.year);
            put(juce::WavAudioFormat::riffInfoGenre, t.genre);
            put(juce::WavAudioFormat::riffInfoComment, t.comment);
            put(juce::WavAudioFormat::riffInfoTrackNumber, t.track);
            // iXML, as JUCE writes it: the ASWG block game and post audio read.
            put(juce::WavAudioFormat::aswgSongTitle, t.title);
            put(juce::WavAudioFormat::aswgArtist, t.artist);
            put(juce::WavAudioFormat::aswgProject, t.album);
            put(juce::WavAudioFormat::aswgGenre, t.genre);
            put(juce::WavAudioFormat::aswgNotes, t.comment);
            if (! t.empty())
            {
                // Broadcast WAV: what it is, who made it, and when.
                put(juce::WavAudioFormat::bwavDescription, (t.title.isNotEmpty() ? t.title : t.comment).substring(0, 256));
                put(juce::WavAudioFormat::bwavOriginator, t.artist.isNotEmpty() ? t.artist.substring(0, 32) : juce::String("SoundSplice"));
                const auto now = juce::Time::getCurrentTime();
                put(juce::WavAudioFormat::bwavOriginationDate, now.formatted("%Y-%m-%d"));
                put(juce::WavAudioFormat::bwavOriginationTime, now.formatted("%H-%M-%S"));
                put(juce::WavAudioFormat::bwavTimeReference, "0");
            }
        }
        if (ogg)
        {
            put(juce::OggVorbisAudioFormat::id3title, t.title);
            put(juce::OggVorbisAudioFormat::id3artist, t.artist);
            put(juce::OggVorbisAudioFormat::id3album, t.album);
            put(juce::OggVorbisAudioFormat::id3date, t.year);
            put(juce::OggVorbisAudioFormat::id3genre, t.genre);
            put(juce::OggVorbisAudioFormat::id3comment, t.comment);
            put(juce::OggVorbisAudioFormat::id3trackNumber, t.track);
        }
        return m;
    }

    namespace detail
    {
        inline void bigEndian(juce::MemoryOutputStream& out, uint32_t v, int bytes = 4)
        {
            for (int i = bytes - 1; i >= 0; --i)
                out.writeByte((char) ((v >> (8 * i)) & 0xff));
        }

        inline void syncsafe(juce::MemoryOutputStream& out, uint32_t v)
        {
            for (int i = 3; i >= 0; --i)
                out.writeByte((char) ((v >> (7 * i)) & 0x7f));
        }

        inline void utf8(juce::MemoryOutputStream& out, const juce::String& s, bool terminate)
        {
            out.write(s.toRawUTF8(), s.getNumBytesAsUTF8());
            if (terminate)
                out.writeByte(0);
        }

        inline juce::MemoryBlock frame(const char* id, const juce::MemoryBlock& body)
        {
            juce::MemoryOutputStream out;
            out.write(id, 4);
            syncsafe(out, (uint32_t) body.getSize());
            out.writeShort(0); // flags
            out.write(body.getData(), body.getSize());
            return out.getMemoryBlock();
        }

        inline juce::MemoryBlock textFrame(const char* id, const juce::String& text)
        {
            juce::MemoryOutputStream body;
            body.writeByte(3); // UTF-8
            utf8(body, text, false);
            return frame(id, body.getMemoryBlock());
        }

        inline juce::String imageMime(const juce::File& image)
        {
            return image.hasFileExtension("png") ? "image/png" : "image/jpeg";
        }

        inline uint32_t milliseconds(double seconds)
        {
            return (uint32_t) juce::jmax(0.0, std::round(seconds * 1000.0));
        }
    }

    /** The ID3v2.4 tag for @p t, or nothing if there's nothing to say. */
    inline juce::MemoryBlock id3v2(const ExportTags& t)
    {
        using namespace detail;
        juce::MemoryOutputStream frames;
        const auto add = [&frames](const juce::MemoryBlock& f) { frames.write(f.getData(), f.getSize()); };

        const std::pair<const char*, const juce::String*> texts[] {
            { "TIT2", &t.title }, { "TPE1", &t.artist }, { "TALB", &t.album }, { "TDRC", &t.year },
            { "TCON", &t.genre }, { "TRCK", &t.track },
        };
        for (const auto& [id, value] : texts)
            if (value->isNotEmpty())
                add(textFrame(id, *value));

        if (t.comment.isNotEmpty())
        {
            juce::MemoryOutputStream body;
            body.writeByte(3);
            body.write("eng", 3);
            body.writeByte(0); // no short description
            utf8(body, t.comment, false);
            add(frame("COMM", body.getMemoryBlock()));
        }

        if (t.coverArt.existsAsFile())
        {
            juce::MemoryBlock image;
            if (t.coverArt.loadFileAsData(image) && image.getSize() > 0)
            {
                juce::MemoryOutputStream body;
                body.writeByte(3);
                utf8(body, imageMime(t.coverArt), true);
                body.writeByte(3); // front cover
                body.writeByte(0); // no description
                body.write(image.getData(), image.getSize());
                add(frame("APIC", body.getMemoryBlock()));
            }
        }

        // Chapters (ID3v2 Chapter Frame Addendum): a CHAP per chapter, each
        // titled, and a CTOC listing them in order.
        const int chapterCount = juce::jmin(255, (int) t.chapters.size());
        if (chapterCount > 0)
        {
            juce::MemoryOutputStream toc;
            utf8(toc, "toc", true);
            toc.writeByte(0x03); // top level, ordered
            toc.writeByte((char) chapterCount);
            for (int i = 0; i < chapterCount; ++i)
            {
                const auto& chapter = t.chapters[(size_t) i];
                const auto  element = "chp" + juce::String(i);
                utf8(toc, element, true);

                juce::MemoryOutputStream chap;
                utf8(chap, element, true);
                bigEndian(chap, milliseconds(chapter.startSeconds));
                bigEndian(chap, milliseconds(chapter.endSeconds));
                bigEndian(chap, 0xffffffffu); // no byte offsets: times only
                bigEndian(chap, 0xffffffffu);
                const auto title = textFrame("TIT2", chapter.title.isNotEmpty() ? chapter.title
                                                                                : "Chapter " + juce::String(i + 1));
                chap.write(title.getData(), title.getSize());
                add(frame("CHAP", chap.getMemoryBlock()));
            }
            add(frame("CTOC", toc.getMemoryBlock()));
        }

        if (frames.getDataSize() == 0)
            return {};

        juce::MemoryOutputStream tag;
        tag.write("ID3", 3);
        tag.writeByte(4); // v2.4
        tag.writeByte(0);
        tag.writeByte(0); // no flags
        syncsafe(tag, (uint32_t) frames.getDataSize());
        tag.write(frames.getData(), frames.getDataSize());
        return tag.getMemoryBlock();
    }

    /** The Vorbis comments for @p t, as "KEY=value". */
    inline juce::StringArray vorbisComments(const ExportTags& t)
    {
        juce::StringArray c;
        const std::pair<const char*, const juce::String*> fields[] {
            { "TITLE", &t.title }, { "ARTIST", &t.artist }, { "ALBUM", &t.album }, { "DATE", &t.year },
            { "GENRE", &t.genre }, { "COMMENT", &t.comment }, { "TRACKNUMBER", &t.track },
        };
        for (const auto& [key, value] : fields)
            if (value->isNotEmpty())
                c.add(juce::String(key) + "=" + *value);

        // The chapter convention the players that read FLAC chapters read.
        for (int i = 0; i < (int) t.chapters.size() && i < 999; ++i)
        {
            const auto& chapter = t.chapters[(size_t) i];
            const auto  number  = juce::String(i + 1).paddedLeft('0', 3);
            const auto  ms      = detail::milliseconds(chapter.startSeconds);
            c.add("CHAPTER" + number + "=" + juce::String(ms / 3600000).paddedLeft('0', 2) + ":"
                  + juce::String((ms / 60000) % 60).paddedLeft('0', 2) + ":" + juce::String((ms / 1000) % 60).paddedLeft('0', 2)
                  + "." + juce::String(ms % 1000).paddedLeft('0', 3));
            c.add("CHAPTER" + number + "NAME=" + (chapter.title.isNotEmpty() ? chapter.title : "Chapter " + juce::String(i + 1)));
        }
        return c;
    }

    /** The cover as a FLAC PICTURE block's body - which is also what Opus and
        Vorbis carry, base64'd, as METADATA_BLOCK_PICTURE. Empty if there's
        no cover, or one too big for a block. */
    inline juce::MemoryBlock pictureBlock(const ExportTags& t)
    {
        juce::MemoryBlock image;
        if (! t.coverArt.existsAsFile() || ! t.coverArt.loadFileAsData(image) || image.getSize() == 0
            || image.getSize() >= (1u << 24) - 1024)
            return {};

        juce::MemoryOutputStream picture; // big-endian
        const auto mime = detail::imageMime(t.coverArt);
        detail::bigEndian(picture, 3); // front cover
        detail::bigEndian(picture, (uint32_t) mime.length());
        picture.write(mime.toRawUTF8(), (size_t) mime.length());
        detail::bigEndian(picture, 0); // no description
        for (int i = 0; i < 4; ++i)
            detail::bigEndian(picture, 0); // width, height, depth, colours: unknown
        detail::bigEndian(picture, (uint32_t) image.getSize());
        picture.write(image.getData(), image.getSize());
        return picture.getMemoryBlock();
    }

    /** @p data with any VORBIS_COMMENT and PICTURE blocks replaced by ones for
        @p t. False if it isn't a FLAC file. */
    inline bool rewriteFlac(juce::MemoryBlock& data, const ExportTags& t)
    {
        const auto* bytes = static_cast<const uint8_t*>(data.getData());
        const auto  size  = data.getSize();
        if (size < 8 || std::memcmp(bytes, "fLaC", 4) != 0)
            return false;

        struct Block
        {
            int    type;
            size_t start, length; // the body
        };
        std::vector<Block> blocks;
        size_t at = 4;
        for (bool last = false; ! last;)
        {
            if (at + 4 > size)
                return false;
            last           = (bytes[at] & 0x80) != 0;
            const int type = bytes[at] & 0x7f;
            const size_t length = ((size_t) bytes[at + 1] << 16) | ((size_t) bytes[at + 2] << 8) | bytes[at + 3];
            if (at + 4 + length > size)
                return false;
            blocks.push_back({ type, at + 4, length });
            at += 4 + length;
        }
        const size_t audioStart = at;

        std::vector<std::pair<int, juce::MemoryBlock>> kept;
        for (const auto& block : blocks)
            if (block.type != 4 && block.type != 6)
                kept.push_back({ block.type, juce::MemoryBlock(bytes + block.start, block.length) });

        // Ours go straight after STREAMINFO (always the first).
        std::vector<std::pair<int, juce::MemoryBlock>> ours;
        {
            juce::MemoryOutputStream comment; // little-endian, as Vorbis is
            const juce::String vendor = "SoundSplice";
            comment.writeInt((int) vendor.getNumBytesAsUTF8());
            comment.write(vendor.toRawUTF8(), vendor.getNumBytesAsUTF8());
            const auto lines = vorbisComments(t);
            comment.writeInt(lines.size());
            for (const auto& line : lines)
            {
                comment.writeInt((int) line.getNumBytesAsUTF8());
                comment.write(line.toRawUTF8(), line.getNumBytesAsUTF8());
            }
            ours.push_back({ 4, comment.getMemoryBlock() });
        }
        if (auto picture = pictureBlock(t); picture.getSize() > 0)
            ours.push_back({ 6, std::move(picture) });
        kept.insert(kept.begin() + (kept.empty() ? 0 : 1), ours.begin(), ours.end());

        juce::MemoryOutputStream out;
        out.write("fLaC", 4);
        for (size_t i = 0; i < kept.size(); ++i)
        {
            const bool last = i + 1 == kept.size();
            out.writeByte((char) ((last ? 0x80 : 0) | kept[i].first));
            detail::bigEndian(out, (uint32_t) kept[i].second.getSize(), 3);
            out.write(kept[i].second.getData(), kept[i].second.getSize());
        }
        out.write(bytes + audioStart, size - audioStart);
        data = out.getMemoryBlock();
        return true;
    }

    /** After the writer has finished: the tags the writer couldn't put in.
        False if the file couldn't be rewritten. */
    inline bool finishFile(const juce::File& file, bool flac, bool mp3, const ExportTags& t)
    {
        if (t.empty() || (! flac && ! mp3))
            return true;

        juce::MemoryBlock data;
        if (! file.loadFileAsData(data))
            return false;

        if (flac)
        {
            if (! rewriteFlac(data, t))
                return false;
        }
        else
        {
            const auto tag = id3v2(t);
            if (tag.getSize() == 0)
                return true;
            juce::MemoryBlock tagged(tag);
            tagged.append(data.getData(), data.getSize());
            data = std::move(tagged);
        }
        return file.replaceWithData(data.getData(), data.getSize());
    }
}

} // namespace soundsplice::engine
