// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include "Archive.h"

#include "GridDescriptor.h"
#include "DelayedLoadMetadata.h"
#include "io.h"

#include <openvdb/Exceptions.h>
#include <openvdb/Metadata.h>
#include <openvdb/tree/LeafManager.h>
#include <openvdb/util/logging.h>
#include <openvdb/openvdb.h>

#ifdef OPENVDB_USE_DELAYED_LOADING
// Boost.Interprocess uses a header-only portion of Boost.DateTime
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-macros"
#endif
#define BOOST_DATE_TIME_NO_LIB
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>

#ifdef _WIN32
#include <boost/interprocess/detail/os_file_functions.hpp> // open_existing_file(), close_file()
extern "C" __declspec(dllimport) bool __stdcall GetFileTime(
    void* fh, void* ctime, void* atime, void* mtime);
// boost::interprocess::detail was renamed to boost::interprocess::ipcdetail in Boost 1.48.
// Ensure that both namespaces exist.
namespace boost { namespace interprocess { namespace detail {} namespace ipcdetail {} } }
#else
#include <sys/types.h> // for struct stat
#include <sys/stat.h> // for stat()
#include <unistd.h> // for unlink()
#endif
#endif // OPENVDB_USE_DELAYED_LOADING

#include <atomic>

#include <algorithm> // for std::find_if()
#include <cerrno> // for errno
#include <cstdlib> // for getenv()
#include <cstring> // for std::memcpy()
#include <ctime> // for std::time()
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <system_error> // for std::error_code()


namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace io {

#ifdef OPENVDB_USE_BLOSC
const uint32_t Archive::DEFAULT_COMPRESSION_FLAGS = (COMPRESS_BLOSC | COMPRESS_ACTIVE_MASK);
#else
#ifdef OPENVDB_USE_ZLIB
const uint32_t Archive::DEFAULT_COMPRESSION_FLAGS = (COMPRESS_ZIP | COMPRESS_ACTIVE_MASK);
#else
const uint32_t Archive::DEFAULT_COMPRESSION_FLAGS = (COMPRESS_ACTIVE_MASK);
#endif
#endif


namespace {

// Indices into a stream's internal extensible array of values used by readers and writers
struct StreamState
{
    static const long MAGIC_NUMBER;

    StreamState();
    ~StreamState();

    // Important:  The size and order of these member variables must *only* change when
    //             OpenVDB ABI changes to avoid potential segfaults when performing I/O
    //             across two different versions of the library. Adding new member
    //             variables to the end of the struct is allowed provided that they
    //             are only accessed from within an appropriate ABI guard.
    int magicNumber;
    int fileVersion;
    int libraryMajorVersion;
    int libraryMinorVersion;
    int dataCompression;
    int writeGridStatsMetadata;
    int gridBackground;
    int gridClass;
    int halfFloat;
    int mappedFile;
    int metadata;
};

inline StreamState& GetSteamState()
{
    static StreamState sStreamState;
    return sStreamState;
}

const long StreamState::MAGIC_NUMBER =
    long((uint64_t(OPENVDB_MAGIC) << 32) | (uint64_t(OPENVDB_MAGIC)));


////////////////////////////////////////


StreamState::StreamState(): magicNumber(std::ios_base::xalloc())
{
    // Having reserved an entry (the one at index magicNumber) in the extensible array
    // associated with every stream, store a magic number at that location in the
    // array belonging to the cout stream.
    std::cout.iword(magicNumber) = MAGIC_NUMBER;
    std::cout.pword(magicNumber) = this;

    // Search for a lower-numbered entry in cout's array that already contains the magic number.
    /// @todo This assumes that the indices returned by xalloc() increase monotonically.
    int existingArray = -1;
    for (int i = 0; i < magicNumber; ++i) {
        if (std::cout.iword(i) == MAGIC_NUMBER) {
            existingArray = i;
            break;
        }
    }

    if (existingArray >= 0 && std::cout.pword(existingArray) != nullptr) {
        // If a lower-numbered entry was found to contain the magic number,
        // a coexisting version of this library must have registered it.
        // In that case, the corresponding pointer should point to an existing
        // StreamState struct.  Copy the other array indices from that StreamState
        // into this one, so as to share state with the other library.
        const StreamState& other =
            *static_cast<const StreamState*>(std::cout.pword(existingArray));
        fileVersion =            other.fileVersion;
        libraryMajorVersion =    other.libraryMajorVersion;
        libraryMinorVersion =    other.libraryMinorVersion;
        dataCompression =        other.dataCompression;
        writeGridStatsMetadata = other.writeGridStatsMetadata;
        gridBackground =         other.gridBackground;
        gridClass =              other.gridClass;
        if (other.mappedFile != 0) { // memory-mapped file support was added in OpenVDB 3.0.0
            mappedFile =         other.mappedFile;
            metadata =           other.metadata;
            halfFloat =          other.halfFloat;
        } else {
            mappedFile =         std::ios_base::xalloc();
            metadata =           std::ios_base::xalloc();
            halfFloat =          std::ios_base::xalloc();
        }
    } else {
        // Reserve storage for per-stream file format and library version numbers
        // and other values of use to readers and writers.  Each of the following
        // values is an index into the extensible arrays associated with all streams.
        // The indices are common to all streams, but the values stored at those indices
        // are unique to each stream.
        fileVersion =            std::ios_base::xalloc();
        libraryMajorVersion =    std::ios_base::xalloc();
        libraryMinorVersion =    std::ios_base::xalloc();
        dataCompression =        std::ios_base::xalloc();
        writeGridStatsMetadata = std::ios_base::xalloc();
        gridBackground =         std::ios_base::xalloc();
        gridClass =              std::ios_base::xalloc();
        mappedFile =             std::ios_base::xalloc();
        metadata =               std::ios_base::xalloc();
        halfFloat =              std::ios_base::xalloc();
    }
}


StreamState::~StreamState()
{
    // Ensure that this StreamState struct can no longer be accessed.
    std::cout.iword(magicNumber) = 0;
    std::cout.pword(magicNumber) = nullptr;
}

} // unnamed namespace


////////////////////////////////////////


struct StreamMetadata::Impl
{
    // Important:  The size and order of these member variables must *only* change when
    //             OpenVDB ABI changes to avoid potential segfaults when performing I/O
    //             across two different versions of the library. Adding new member
    //             variables to the end of the struct is allowed provided that they
    //             are only accessed from within an appropriate ABI guard.

    uint32_t mFileVersion = OPENVDB_FILE_VERSION;
    VersionId mLibraryVersion = { OPENVDB_LIBRARY_MAJOR_VERSION, OPENVDB_LIBRARY_MINOR_VERSION };
    uint32_t mCompression = COMPRESS_NONE;
    uint32_t mGridClass = GRID_UNKNOWN;
    const void* mBackgroundPtr = nullptr; ///< @todo use Metadata::Ptr?
    bool mHalfFloat = false;
    bool mWriteGridStats = false;
    bool mSeekable = false;
    bool mCountingPasses = false;
    uint32_t mPass = 0;
    MetaMap mGridMetadata;
    AuxDataMap mAuxData;
    bool mDelayedLoadMeta = DelayedLoadMetadata::isRegisteredType();
    uint64_t mLeaf = 0;
    uint32_t mTest = 0; // for testing only
}; // struct StreamMetadata


StreamMetadata::StreamMetadata(): mImpl(new Impl)
{
}


StreamMetadata::StreamMetadata(const StreamMetadata& other): mImpl(new Impl(*other.mImpl))
{
}


StreamMetadata::StreamMetadata(std::ios_base& strm): mImpl(new Impl)
{
    mImpl->mFileVersion = getFormatVersion(strm);
    mImpl->mLibraryVersion = getLibraryVersion(strm);
    mImpl->mCompression = getDataCompression(strm);
    mImpl->mGridClass = getGridClass(strm);
    mImpl->mHalfFloat = getHalfFloat(strm);
    mImpl->mWriteGridStats = getWriteGridStatsMetadata(strm);
}


StreamMetadata::~StreamMetadata()
{
}


StreamMetadata&
StreamMetadata::operator=(const StreamMetadata& other)
{
    if (&other != this) {
        mImpl.reset(new Impl(*other.mImpl));
    }
    return *this;
}


void
StreamMetadata::transferTo(std::ios_base& strm) const
{
    io::setVersion(strm, mImpl->mLibraryVersion, mImpl->mFileVersion);
    io::setDataCompression(strm, mImpl->mCompression);
    io::setGridBackgroundValuePtr(strm, mImpl->mBackgroundPtr);
    io::setGridClass(strm, mImpl->mGridClass);
    io::setHalfFloat(strm, mImpl->mHalfFloat);
    io::setWriteGridStatsMetadata(strm, mImpl->mWriteGridStats);
}


uint32_t        StreamMetadata::fileVersion() const     { return mImpl->mFileVersion; }
VersionId       StreamMetadata::libraryVersion() const  { return mImpl->mLibraryVersion; }
uint32_t        StreamMetadata::compression() const     { return mImpl->mCompression; }
uint32_t        StreamMetadata::gridClass() const       { return mImpl->mGridClass; }
const void*     StreamMetadata::backgroundPtr() const   { return mImpl->mBackgroundPtr; }
bool            StreamMetadata::halfFloat() const       { return mImpl->mHalfFloat; }
bool            StreamMetadata::writeGridStats() const  { return mImpl->mWriteGridStats; }
bool            StreamMetadata::seekable() const        { return mImpl->mSeekable; }
bool            StreamMetadata::delayedLoadMeta() const { return mImpl->mDelayedLoadMeta; }
bool            StreamMetadata::countingPasses() const  { return mImpl->mCountingPasses; }
uint32_t        StreamMetadata::pass() const            { return mImpl->mPass; }
uint64_t        StreamMetadata::leaf() const            { return mImpl->mLeaf; }
MetaMap&        StreamMetadata::gridMetadata()          { return mImpl->mGridMetadata; }
const MetaMap&  StreamMetadata::gridMetadata() const    { return mImpl->mGridMetadata; }
uint32_t        StreamMetadata::__test() const          { return mImpl->mTest; }

StreamMetadata::AuxDataMap& StreamMetadata::auxData() { return mImpl->mAuxData; }
const StreamMetadata::AuxDataMap& StreamMetadata::auxData() const { return mImpl->mAuxData; }

void StreamMetadata::setFileVersion(uint32_t v)         { mImpl->mFileVersion = v; }
void StreamMetadata::setLibraryVersion(VersionId v)     { mImpl->mLibraryVersion = v; }
void StreamMetadata::setCompression(uint32_t c)         { mImpl->mCompression = c; }
void StreamMetadata::setGridClass(uint32_t c)           { mImpl->mGridClass = c; }
void StreamMetadata::setBackgroundPtr(const void* ptr)  { mImpl->mBackgroundPtr = ptr; }
void StreamMetadata::setHalfFloat(bool b)               { mImpl->mHalfFloat = b; }
void StreamMetadata::setWriteGridStats(bool b)          { mImpl->mWriteGridStats = b; }
void StreamMetadata::setSeekable(bool b)                { mImpl->mSeekable = b; }
void StreamMetadata::setCountingPasses(bool b)          { mImpl->mCountingPasses = b; }
void StreamMetadata::setPass(uint32_t i)                { mImpl->mPass = i; }
void StreamMetadata::setLeaf(uint64_t i)                { mImpl->mLeaf = i; }
void StreamMetadata::__setTest(uint32_t t)              { mImpl->mTest = t; }

std::string
StreamMetadata::str() const
{
    std::ostringstream ostr;
    ostr << std::boolalpha;
    ostr << "version: " << libraryVersion().first << "." << libraryVersion().second
        << "/" << fileVersion() << "\n";
    ostr << "class: " << GridBase::gridClassToString(static_cast<GridClass>(gridClass())) << "\n";
    ostr << "compression: " << compressionToString(compression()) << "\n";
    ostr << "half_float: " << halfFloat() << "\n";
    ostr << "seekable: " << seekable() << "\n";
    ostr << "delayed_load_meta: " << delayedLoadMeta() << "\n";
    ostr << "pass: " << pass() << "\n";
    ostr << "counting_passes: " << countingPasses() << "\n";
    ostr << "write_grid_stats_metadata: " << writeGridStats() << "\n";
    if (!auxData().empty()) ostr << auxData();
    if (gridMetadata().metaCount() != 0) {
        ostr << "grid_metadata:\n" << gridMetadata().str(/*indent=*/"    ");
    }
    return ostr.str();
}


std::ostream&
operator<<(std::ostream& os, const StreamMetadata& meta)
{
    os << meta.str();
    return os;
}


namespace {

template<typename T>
inline bool
writeAsType(std::ostream& os, const std::any& val)
{
    if (val.type() == typeid(T)) {
        os << std::any_cast<T>(val);
        return true;
    }
    return false;
}

struct PopulateDelayedLoadMetadataOp
{
    DelayedLoadMetadata& metadata;
    uint32_t compression;

    PopulateDelayedLoadMetadataOp(DelayedLoadMetadata& _metadata, uint32_t _compression)
        : metadata(_metadata)
        , compression(_compression) { }

    template<typename GridT>
    void operator()(const GridT& grid) const
    {
        using TreeT = typename GridT::TreeType;
        using ValueT = typename TreeT::ValueType;
        using LeafT = typename TreeT::LeafNodeType;
        using MaskT = typename LeafT::NodeMaskType;

        const TreeT& tree = grid.constTree();
        const Index64 leafCount = tree.leafCount();

        // early exit if not leaf nodes
        if (leafCount == Index64(0))    return;

        metadata.resizeMask(leafCount);

        if (compression & (COMPRESS_BLOSC | COMPRESS_ZIP)) {
            metadata.resizeCompressedSize(leafCount);
        }

        const auto background = tree.background();
        const bool saveFloatAsHalf = grid.saveFloatAsHalf();

        tree::LeafManager<const TreeT> leafManager(tree);

        leafManager.foreach(
            [&](const LeafT& leaf, size_t idx) {
                // set mask value
                MaskCompress<ValueT, MaskT> maskCompressData(
                    leaf.valueMask(), /*childMask=*/MaskT(), leaf.buffer().data(), background);
                metadata.setMask(idx, maskCompressData.metadata);

                if (compression & (COMPRESS_BLOSC | COMPRESS_ZIP)) {
                    // set compressed size value
                    size_t sizeBytes(8);
                    size_t compressedSize = io::writeCompressedValuesSize(
                        leaf.buffer().data(), LeafT::SIZE,
                        leaf.valueMask(), maskCompressData.metadata, saveFloatAsHalf, compression);
                    metadata.setCompressedSize(idx, compressedSize+sizeBytes);
                }
            }
        );
    }
};

bool populateDelayedLoadMetadata(DelayedLoadMetadata& metadata,
    const GridBase& gridBase, uint32_t compression)
{
    PopulateDelayedLoadMetadataOp op(metadata, compression);

    using AllowedTypes = TypeList<
        Int32Grid, Int64Grid,
        FloatGrid, DoubleGrid,
        Vec3IGrid, Vec3SGrid, Vec3DGrid>;

    return gridBase.apply<AllowedTypes>(op);
}

} // unnamed namespace

std::ostream&
operator<<(std::ostream& os, const StreamMetadata::AuxDataMap& auxData)
{
    for (StreamMetadata::AuxDataMap::const_iterator it = auxData.begin(), end = auxData.end();
        it != end; ++it)
    {
        os << it->first << ": ";
        // Note: std::any doesn't support serialization.
        const std::any& val = it->second;
        if (!writeAsType<int32_t>(os, val)
            && !writeAsType<int64_t>(os, val)
            && !writeAsType<int16_t>(os, val)
            && !writeAsType<int8_t>(os, val)
            && !writeAsType<uint32_t>(os, val)
            && !writeAsType<uint64_t>(os, val)
            && !writeAsType<uint16_t>(os, val)
            && !writeAsType<uint8_t>(os, val)
            && !writeAsType<float>(os, val)
            && !writeAsType<double>(os, val)
            && !writeAsType<long double>(os, val)
            && !writeAsType<bool>(os, val)
            && !writeAsType<std::string>(os, val)
            && !writeAsType<const char*>(os, val))
        {
            os << val.type().name() << "(...)";
        }
        os << "\n";
    }
    return os;
}


////////////////////////////////////////


#ifdef OPENVDB_USE_DELAYED_LOADING


// Memory-mapping a VDB file permits threaded input (and output, potentially,
// though that might not be practical for compressed files or files containing
// multiple grids).  In particular, a memory-mapped file can be loaded lazily,
// meaning that the voxel buffers of the leaf nodes of a grid's tree are not allocated
// until they are actually accessed.  When access to its buffer is requested,
// a leaf node allocates memory for the buffer and then streams in (and decompresses)
// its contents from the memory map, starting from a stream offset that was recorded
// at the time the node was constructed.  The memory map must persist as long as
// there are unloaded leaf nodes; this is ensured by storing a shared pointer
// to the map in each unloaded node.

class MappedFile::Impl
{
public:
    Impl(const std::string& filename, bool autoDelete)
        : mMap(filename.c_str(), boost::interprocess::read_only)
        , mRegion(mMap, boost::interprocess::read_only)
        , mAutoDelete(autoDelete)
    {
        if (mAutoDelete) {
#ifndef _WIN32
            // On Unix systems, unlink the file so that it gets deleted once it is closed.
            ::unlink(mMap.get_name());
#endif
        }
    }

    ~Impl()
    {
        std::string filename;
        if (const char* s = mMap.get_name()) filename = s;
        OPENVDB_LOG_DEBUG_RUNTIME("closing memory-mapped file " << filename);
        if (mNotifier) mNotifier(filename);
        if (mAutoDelete) {
            if (!boost::interprocess::file_mapping::remove(filename.c_str())) {
                if (errno != ENOENT) {
                    // Warn if the file exists but couldn't be removed.
                    std::string mesg = getErrorString();
                    if (!mesg.empty()) mesg = " (" + mesg + ")";
                    OPENVDB_LOG_WARN("failed to remove temporary file " << filename << mesg);
                }
            }
        }
    }

    boost::interprocess::file_mapping mMap;
    boost::interprocess::mapped_region mRegion;
    bool mAutoDelete;
    Notifier mNotifier;
#if OPENVDB_ABI_VERSION_NUMBER <= 12
    mutable std::atomic<Index64> mLastWriteTime;
#endif

private:
    Impl(const Impl&); // not copyable
    Impl& operator=(const Impl&); // not copyable
};


MappedFile::MappedFile(const std::string& filename, bool autoDelete):
    mImpl(new Impl(filename, autoDelete))
{
}


MappedFile::~MappedFile()
{
}


std::string
MappedFile::filename() const
{
    std::string result;
    if (const char* s = mImpl->mMap.get_name()) result = s;
    return result;
}


SharedPtr<std::streambuf>
MappedFile::createBuffer() const
{
    return SharedPtr<std::streambuf>{
        new boost::iostreams::stream_buffer<boost::iostreams::array_source>{
            static_cast<const char*>(mImpl->mRegion.get_address()), mImpl->mRegion.get_size()}};
}


void
MappedFile::setNotifier(const Notifier& notifier)
{
    mImpl->mNotifier = notifier;
}


void
MappedFile::clearNotifier()
{
    mImpl->mNotifier = nullptr;
}


#endif // OPENVDB_USE_DELAYED_LOADING


////////////////////////////////////////


std::string
getErrorString(int errorNum)
{
    return std::error_code(errorNum, std::generic_category()).message();
}


std::string
getErrorString()
{
    return getErrorString(errno);
}


////////////////////////////////////////


Archive::Archive()
    : mFileVersion(OPENVDB_FILE_VERSION)
    , mLibraryVersion(OPENVDB_LIBRARY_MAJOR_VERSION, OPENVDB_LIBRARY_MINOR_VERSION)
    , mUuid()
    , mInputHasGridOffsets(false)
    , mEnableInstancing(true)
    , mCompression(DEFAULT_COMPRESSION_FLAGS)
    , mEnableGridStats(true)
{
}


Archive::~Archive()
{
}


Archive::Ptr
Archive::copy() const
{
    return Archive::Ptr(new Archive(*this));
}


////////////////////////////////////////


std::string
Archive::getUniqueTag() const
{
    return mUuid;
}


bool
Archive::isIdentical(const std::string& uuidStr) const
{
    // If either uuids are blank, they will always fail to match
    // as something went wrong generating them.
    if (uuidStr.empty()) return false;
    if (getUniqueTag().empty()) return false;
    return uuidStr == getUniqueTag();
}


////////////////////////////////////////


uint32_t
getFormatVersion(std::ios_base& is)
{
    /// @todo get from StreamMetadata
    return static_cast<uint32_t>(is.iword(GetSteamState().fileVersion));
}


void
Archive::setFormatVersion(std::istream& is)
{
    is.iword(GetSteamState().fileVersion) = mFileVersion; ///< @todo remove
    if (StreamMetadata::Ptr meta = getStreamMetadataPtr(is)) {
        meta->setFileVersion(mFileVersion);
    }
}


VersionId
getLibraryVersion(std::ios_base& is)
{
    /// @todo get from StreamMetadata
    VersionId version;
    version.first = static_cast<uint32_t>(is.iword(GetSteamState().libraryMajorVersion));
    version.second = static_cast<uint32_t>(is.iword(GetSteamState().libraryMinorVersion));
    return version;
}


void
Archive::setLibraryVersion(std::istream& is)
{
    is.iword(GetSteamState().libraryMajorVersion) = mLibraryVersion.first; ///< @todo remove
    is.iword(GetSteamState().libraryMinorVersion) = mLibraryVersion.second; ///< @todo remove
    if (StreamMetadata::Ptr meta = getStreamMetadataPtr(is)) {
        meta->setLibraryVersion(mLibraryVersion);
    }
}


std::string
getVersion(std::ios_base& is)
{
    VersionId version = getLibraryVersion(is);
    std::ostringstream ostr;
    ostr << version.first << "." << version.second << "/" << getFormatVersion(is);
    return ostr.str();
}


void
setCurrentVersion(std::istream& is)
{
    is.iword(GetSteamState().fileVersion) = OPENVDB_FILE_VERSION; ///< @todo remove
    is.iword(GetSteamState().libraryMajorVersion) = OPENVDB_LIBRARY_MAJOR_VERSION; ///< @todo remove
    is.iword(GetSteamState().libraryMinorVersion) = OPENVDB_LIBRARY_MINOR_VERSION; ///< @todo remove
    if (StreamMetadata::Ptr meta = getStreamMetadataPtr(is)) {
        meta->setFileVersion(OPENVDB_FILE_VERSION);
        meta->setLibraryVersion(VersionId(
            OPENVDB_LIBRARY_MAJOR_VERSION, OPENVDB_LIBRARY_MINOR_VERSION));
    }
}


void
setVersion(std::ios_base& strm, const VersionId& libraryVersion, uint32_t fileVersion)
{
    strm.iword(GetSteamState().fileVersion) = fileVersion; ///< @todo remove
    strm.iword(GetSteamState().libraryMajorVersion) = libraryVersion.first; ///< @todo remove
    strm.iword(GetSteamState().libraryMinorVersion) = libraryVersion.second; ///< @todo remove
    if (StreamMetadata::Ptr meta = getStreamMetadataPtr(strm)) {
        meta->setFileVersion(fileVersion);
        meta->setLibraryVersion(libraryVersion);
    }
}


std::string
Archive::version() const
{
    std::ostringstream ostr;
    ostr << mLibraryVersion.first << "." << mLibraryVersion.second << "/" << mFileVersion;
    return ostr.str();
}


////////////////////////////////////////


uint32_t
getDataCompression(std::ios_base& strm)
{
    /// @todo get from StreamMetadata
    return uint32_t(strm.iword(GetSteamState().dataCompression));
}


void
setDataCompression(std::ios_base& strm, uint32_t c)
{
    strm.iword(GetSteamState().dataCompression) = c; ///< @todo remove
    if (StreamMetadata::Ptr meta = getStreamMetadataPtr(strm)) {
        meta->setCompression(c);
    }
}


void
Archive::setDataCompression(std::istream& is)
{
    io::setDataCompression(is, mCompression); ///< @todo remove
    if (StreamMetadata::Ptr meta = getStreamMetadataPtr(is)) {
        meta->setCompression(mCompression);
    }
}


//static
bool
Archive::hasBloscCompression()
{
#ifdef OPENVDB_USE_BLOSC
    return true;
#else
    return false;
#endif
}


//static
bool
Archive::hasZLibCompression()
{
#ifdef OPENVDB_USE_ZLIB
    return true;
#else
    return false;
#endif
}


void
Archive::setGridCompression(std::ostream& os, const GridBase& grid) const
{
    // Start with the options that are enabled globally for this archive.
    uint32_t c = compression();

    // Disable options that are inappropriate for the given grid.
    switch (grid.getGridClass()) {
        case GRID_LEVEL_SET:
        case GRID_FOG_VOLUME:
            // ZLIB compression is not used on level sets or fog volumes.
            c = c & ~COMPRESS_ZIP;
            break;
        case GRID_STAGGERED:
        case GRID_UNKNOWN:
            break;
    }
    io::setDataCompression(os, c);

    os.write(reinterpret_cast<const char*>(&c), sizeof(uint32_t));
}


void
Archive::readGridCompression(std::istream& is)
{
    if (getFormatVersion(is) >= OPENVDB_FILE_VERSION_NODE_MASK_COMPRESSION) {
        uint32_t c = COMPRESS_NONE;
        is.read(reinterpret_cast<char*>(&c), sizeof(uint32_t));
        io::setDataCompression(is, c);
    }
}


////////////////////////////////////////


bool
getWriteGridStatsMetadata(std::ios_base& strm)
{
    /// @todo get from StreamMetadata
    return strm.iword(GetSteamState().writeGridStatsMetadata) != 0;
}


void
setWriteGridStatsMetadata(std::ios_base& strm, bool writeGridStats)
{
    strm.iword(GetSteamState().writeGridStatsMetadata) = writeGridStats; ///< @todo remove
    if (StreamMetadata::Ptr meta = getStreamMetadataPtr(strm)) {
        meta->setWriteGridStats(writeGridStats);
    }
}


////////////////////////////////////////


uint32_t
getGridClass(std::ios_base& strm)
{
    /// @todo get from StreamMetadata
    const uint32_t val = static_cast<uint32_t>(strm.iword(GetSteamState().gridClass));
    if (val >= NUM_GRID_CLASSES) return GRID_UNKNOWN;
    return val;
}


void
setGridClass(std::ios_base& strm, uint32_t cls)
{
    strm.iword(GetSteamState().gridClass) = long(cls); ///< @todo remove
    if (StreamMetadata::Ptr meta = getStreamMetadataPtr(strm)) {
        meta->setGridClass(cls);
    }
}


bool
getHalfFloat(std::ios_base& strm)
{
    /// @todo get from StreamMetadata
    return strm.iword(GetSteamState().halfFloat) != 0;
}


void
setHalfFloat(std::ios_base& strm, bool halfFloat)
{
    strm.iword(GetSteamState().halfFloat) = halfFloat; ///< @todo remove
    if (StreamMetadata::Ptr meta = getStreamMetadataPtr(strm)) {
        meta->setHalfFloat(halfFloat);
    }
}


const void*
getGridBackgroundValuePtr(std::ios_base& strm)
{
    /// @todo get from StreamMetadata
    return strm.pword(GetSteamState().gridBackground);
}


void
setGridBackgroundValuePtr(std::ios_base& strm, const void* background)
{
    strm.pword(GetSteamState().gridBackground) = const_cast<void*>(background); ///< @todo remove
    if (StreamMetadata::Ptr meta = getStreamMetadataPtr(strm)) {
        meta->setBackgroundPtr(background);
    }
}


#ifdef OPENVDB_USE_DELAYED_LOADING
MappedFile::Ptr
getMappedFilePtr(std::ios_base& strm)
{
    if (const void* ptr = strm.pword(GetSteamState().mappedFile)) {
        return *static_cast<const MappedFile::Ptr*>(ptr);
    }
    return MappedFile::Ptr();
}


void
setMappedFilePtr(std::ios_base& strm, io::MappedFile::Ptr& mappedFile)
{
    strm.pword(GetSteamState().mappedFile) = &mappedFile;
}
#endif // OPENVDB_USE_DELAYED_LOADING


StreamMetadata::Ptr
getStreamMetadataPtr(std::ios_base& strm)
{
    if (const void* ptr = strm.pword(GetSteamState().metadata)) {
        return *static_cast<const StreamMetadata::Ptr*>(ptr);
    }
    return StreamMetadata::Ptr();
}


void
setStreamMetadataPtr(std::ios_base& strm, StreamMetadata::Ptr& meta, bool transfer)
{
    strm.pword(GetSteamState().metadata) = &meta;
    if (transfer && meta) meta->transferTo(strm);
}


StreamMetadata::Ptr
clearStreamMetadataPtr(std::ios_base& strm)
{
    StreamMetadata::Ptr result = getStreamMetadataPtr(strm);
    strm.pword(GetSteamState().metadata) = nullptr;
    return result;
}


////////////////////////////////////////


bool
Archive::readHeader(std::istream& is)
{
    // 1) Read the magic number for VDB.
    int64_t magic;
    is.read(reinterpret_cast<char*>(&magic), sizeof(int64_t));

    if (magic != OPENVDB_MAGIC) {
        OPENVDB_THROW(IoError, "not a VDB file");
    }

    // 2) Read the file format version number.
    is.read(reinterpret_cast<char*>(&mFileVersion), sizeof(uint32_t));
    if (mFileVersion > OPENVDB_FILE_VERSION) {
        OPENVDB_LOG_WARN("unsupported VDB file format (expected version "
            << OPENVDB_FILE_VERSION << " or earlier, got version " << mFileVersion << ")");
    } else if (mFileVersion < 211) {
        // Versions prior to 211 stored separate major, minor and patch numbers.
        uint32_t version;
        is.read(reinterpret_cast<char*>(&version), sizeof(uint32_t));
        mFileVersion = 100 * mFileVersion + 10 * version;
        is.read(reinterpret_cast<char*>(&version), sizeof(uint32_t));
        mFileVersion += version;
    }

    // 3) Read the library version numbers (not stored prior to file format version 211).
    mLibraryVersion.first = mLibraryVersion.second = 0;
    if (mFileVersion >= 211) {
        uint32_t version;
        is.read(reinterpret_cast<char*>(&version), sizeof(uint32_t));
        mLibraryVersion.first = version; // major version
        is.read(reinterpret_cast<char*>(&version), sizeof(uint32_t));
        mLibraryVersion.second = version; // minor version
    }

    // 4) Read the flag indicating whether the stream supports partial reading.
    //    (Versions prior to 212 have no flag because they always supported partial reading.)
    mInputHasGridOffsets = true;
    if (mFileVersion >= 212) {
        char hasGridOffsets;
        is.read(&hasGridOffsets, sizeof(char));
        mInputHasGridOffsets = hasGridOffsets;
    }

    // 5) Read the flag that indicates whether data is compressed.
    //    (From version 222 on, compression information is stored per grid.)
    mCompression = DEFAULT_COMPRESSION_FLAGS;
    if (mFileVersion < OPENVDB_FILE_VERSION_BLOSC_COMPRESSION) {
        // Prior to the introduction of Blosc, ZLIB was the default compression scheme.
        mCompression = (COMPRESS_ZIP | COMPRESS_ACTIVE_MASK);
    }
    if (mFileVersion >= OPENVDB_FILE_VERSION_SELECTIVE_COMPRESSION &&
        mFileVersion < OPENVDB_FILE_VERSION_NODE_MASK_COMPRESSION)
    {
        char isCompressed;
        is.read(&isCompressed, sizeof(char));
        mCompression = (isCompressed != 0 ? COMPRESS_ZIP : COMPRESS_NONE);
    }

    // 6) Read the 16-byte (128-bit) uuid.
    std::string oldUuid = mUuid;
    if (mFileVersion >= OPENVDB_FILE_VERSION_BOOST_UUID) {
        // UUID is stored as fixed-length ASCII string
        // The extra 4 bytes are for the hyphens.
        char uuidValues[16*2+4+1];
        is.read(uuidValues, 16*2+4);
        uuidValues[16*2+4] = 0;
        mUuid = uuidValues;
    } else {
        // Older versions stored the UUID as a byte string.
        char uuidBytes[16];
        is.read(uuidBytes, 16);
        char uuidStr[33];
        auto to_hex = [](unsigned int c) -> char
        {
            c &= 0xf;
            if (c < 10) return (char)('0' + c);
            return (char)(c - 10 + 'A');
        };
        for (int i = 0; i < 16; i++)
        {
            uuidStr[i*2] = to_hex(uuidBytes[i] >> 4);
            uuidStr[i*2+1] = to_hex(uuidBytes[i]);
        }
        uuidStr[32] = 0;
        mUuid = uuidStr;
    }

    // CHeck if new and old uuid differ.  If either are blank, they
    // differ because an error occurred.
    if (oldUuid.empty() || mUuid.empty()) return true;
    return oldUuid != mUuid; // true if UUID in input stream differs from old UUID
}


void
Archive::writeHeader(std::ostream& os, bool seekable) const
{
    // 1) Write the magic number for VDB.
    int64_t magic = OPENVDB_MAGIC;
    os.write(reinterpret_cast<char*>(&magic), sizeof(int64_t));

    // 2) Write the file format version number.
    uint32_t version = OPENVDB_FILE_VERSION;
    os.write(reinterpret_cast<char*>(&version), sizeof(uint32_t));

    // 3) Write the library version numbers.
    version = OPENVDB_LIBRARY_MAJOR_VERSION;
    os.write(reinterpret_cast<char*>(&version), sizeof(uint32_t));
    version = OPENVDB_LIBRARY_MINOR_VERSION;
    os.write(reinterpret_cast<char*>(&version), sizeof(uint32_t));

    // 4) Write a flag indicating that this stream contains no grid offsets.
    char hasGridOffsets = seekable;
    os.write(&hasGridOffsets, sizeof(char));

    // 5) Write a flag indicating that this stream contains compressed leaf data.
    //    (Omitted as of version 222)

    // 6) Generate a new random 16-byte (128-bit) sequence and write it to the stream.

    char uuidStr[16*2+4+1];
    auto to_hex = [](unsigned int c) -> char
    {
        c &= 0xf;
        if (c < 10) return (char)('0' + c);
        return (char)(c - 10 + 'A');
    };

    try
    {
        std::random_device  seed;
        for (int i = 0; i < 4; i++)
        {
            unsigned int v = seed();
            // This writes out in reverse direction of bit order, but
            // as source is random we don't mind.
            for (int j = 0; j < 8; j++)
            {
                uuidStr[i*8+j] = to_hex(v);
                v >>= 4;
            }
        }
    }
    catch (std::exception&)
    {
        // We could have failed due to running out of entropy, but hopefully
        // most platforms use /dev/urandom equivalent...
        // Create a blank UUID that means it should always fail comparisons.
        uuidStr[0] = 0;
    }
    // Insert our hyphens.
    for (int i = 0; i < 4; i++)
        uuidStr[16*2+i] = '-';
    std::swap(uuidStr[16*2+0], uuidStr[8+0]);
    std::swap(uuidStr[16*2+1], uuidStr[12+1]);
    std::swap(uuidStr[16*2+2], uuidStr[16+2]);
    std::swap(uuidStr[16*2+3], uuidStr[20+3]);
    uuidStr[16*2+4] = 0;
    mUuid = uuidStr; // mUuid is mutable
    // We don't write a string; but instead a fixed length buffer.
    // To match the old UUID, we need an extra 4 bytes for hyphens.
    for (int i = 0; i < 16*2+4; i++)
    {
        os << uuidStr[i];
    }
}


////////////////////////////////////////


int32_t
Archive::readGridCount(std::istream& is)
{
    int32_t gridCount = 0;
    is.read(reinterpret_cast<char*>(&gridCount), sizeof(int32_t));
    return gridCount;
}


////////////////////////////////////////


void
Archive::connectInstance(const GridDescriptor& gd, const NamedGridMap& grids) const
{
    if (!gd.isInstance() || grids.empty()) return;

    NamedGridMap::const_iterator it = grids.find(gd.uniqueName());
    if (it == grids.end()) return;
    GridBase::Ptr grid = it->second;
    if (!grid) return;

    it = grids.find(gd.instanceParentName());
    if (it != grids.end()) {
        GridBase::Ptr parent = it->second;
        if (mEnableInstancing) {
            // Share the instance parent's tree.
            grid->setTree(parent->baseTreePtr());
        } else {
            // Copy the instance parent's tree.
            grid->setTree(parent->baseTree().copy());
        }
    } else {
        OPENVDB_THROW(KeyError, "missing instance parent \""
            << GridDescriptor::nameAsString(gd.instanceParentName())
            << "\" for grid " << GridDescriptor::nameAsString(gd.uniqueName()));
    }
}


////////////////////////////////////////


//static
bool
Archive::isDelayedLoadingEnabled()
{
#ifdef OPENVDB_USE_DELAYED_LOADING
    return (nullptr == std::getenv("OPENVDB_DISABLE_DELAYED_LOAD"));
#else
    return false;
#endif
}


namespace {

struct NoBBox {};

template<typename BoxType>
void
doReadGrid(GridBase::Ptr grid, const GridDescriptor& gd, std::istream& is, const BoxType& bbox)
{
    struct Local {
        static void readBuffers(GridBase& g, std::istream& istrm, NoBBox) { g.readBuffers(istrm); }
        static void readBuffers(GridBase& g, std::istream& istrm, const CoordBBox& indexBBox) {
            g.readBuffers(istrm, indexBBox);
        }
        static void readBuffers(GridBase& g, std::istream& istrm, const BBoxd& worldBBox) {
            g.readBuffers(istrm, g.constTransform().worldToIndexNodeCentered(worldBBox));
        }
    };

    // Restore the file-level stream metadata on exit.
    struct OnExit {
        OnExit(std::ios_base& strm_): strm(&strm_), ptr(strm_.pword(GetSteamState().metadata)) {}
        ~OnExit() { strm->pword(GetSteamState().metadata) = ptr; }
        std::ios_base* strm;
        void* ptr;
    };
    OnExit restore(is);

    // Stream metadata varies per grid, and it needs to persist
    // in case delayed load is in effect.
    io::StreamMetadata::Ptr streamMetadata;
    if (io::StreamMetadata::Ptr meta = io::getStreamMetadataPtr(is)) {
        // Make a grid-level copy of the file-level stream metadata.
        streamMetadata.reset(new StreamMetadata(*meta));
    } else {
        streamMetadata.reset(new StreamMetadata);
    }
    streamMetadata->setHalfFloat(grid->saveFloatAsHalf());
    io::setStreamMetadataPtr(is, streamMetadata, /*transfer=*/false);

    io::setGridClass(is, GRID_UNKNOWN);
    io::setGridBackgroundValuePtr(is, nullptr);

    grid->readMeta(is);

    // Add a description of the compression settings to the grid as metadata.
    /// @todo Would this be useful?
    //const uint32_t c = getDataCompression(is);
    //grid->insertMeta(GridBase::META_FILE_COMPRESSION,
    //    StringMetadata(compressionToString(c)));

    const VersionId version = getLibraryVersion(is);
    if (version.first < 6 || (version.first == 6 && version.second <= 1)) {
        // If delay load metadata exists, but the file format version does not support
        // delay load metadata, this likely means the original grid was read and then
        // written using a prior version of OpenVDB and ABI>=5 where unknown metadata
        // can be blindly copied. This means that it is possible for the metadata to
        // no longer be in sync with the grid, so we remove it to ensure correctness.

        if ((*grid)[GridBase::META_FILE_DELAYED_LOAD]) {
            grid->removeMeta(GridBase::META_FILE_DELAYED_LOAD);
        }
    }

    streamMetadata->gridMetadata() = static_cast<MetaMap&>(*grid);
    const GridClass gridClass = grid->getGridClass();
    io::setGridClass(is, gridClass);

    // reset leaf value to zero
    streamMetadata->setLeaf(0);

    // drop DelayedLoadMetadata from the grid as it is only useful for IO
    // a stream metadata non-zero value disables this behaviour for testing

    if (streamMetadata->__test() == uint32_t(0)) {
        if ((*grid)[GridBase::META_FILE_DELAYED_LOAD]) {
            grid->removeMeta(GridBase::META_FILE_DELAYED_LOAD);
        }
    }

    if (getFormatVersion(is) >= OPENVDB_FILE_VERSION_GRID_INSTANCING) {
        grid->readTransform(is);
        if (!gd.isInstance()) {
            grid->readTopology(is);
            Local::readBuffers(*grid, is, bbox);
        }
    } else {
        // Older versions of the library stored the transform after the topology.
        grid->readTopology(is);
        grid->readTransform(is);
        Local::readBuffers(*grid, is, bbox);
    }
    if (getFormatVersion(is) < OPENVDB_FILE_VERSION_NO_GRIDMAP) {
        // Older versions of the library didn't store grid names as metadata,
        // so when reading older files, copy the grid name from the descriptor
        // to the grid's metadata.
        if (grid->getName().empty()) {
            grid->setName(gd.gridName());
        }
    }
}

} // unnamed namespace


void
Archive::readGrid(GridBase::Ptr grid, const GridDescriptor& gd, std::istream& is)
{
    // Read the compression settings for this grid and tag the stream with them
    // so that downstream functions can reference them.
    readGridCompression(is);

    doReadGrid(grid, gd, is, NoBBox());
}

void
Archive::readGrid(GridBase::Ptr grid, const GridDescriptor& gd,
    std::istream& is, const BBoxd& worldBBox)
{
    readGridCompression(is);
    doReadGrid(grid, gd, is, worldBBox);
}

void
Archive::readGrid(GridBase::Ptr grid, const GridDescriptor& gd,
    std::istream& is, const CoordBBox& indexBBox)
{
    readGridCompression(is);
    doReadGrid(grid, gd, is, indexBBox);
}


////////////////////////////////////////


void
Archive::write(std::ostream& os, const GridPtrVec& grids, bool seekable,
    const MetaMap& metadata) const
{
    this->write(os, GridCPtrVec(grids.begin(), grids.end()), seekable, metadata);
}


void
Archive::write(std::ostream& os, const GridCPtrVec& grids, bool seekable,
    const MetaMap& metadata) const
{
    // Set stream flags so that downstream functions can reference them.
    io::StreamMetadata::Ptr streamMetadata = io::getStreamMetadataPtr(os);
    if (!streamMetadata) {
        streamMetadata.reset(new StreamMetadata);
        io::setStreamMetadataPtr(os, streamMetadata, /*transfer=*/false);
    }
    io::setDataCompression(os, compression());
    io::setWriteGridStatsMetadata(os, isGridStatsMetadataEnabled());

    this->writeHeader(os, seekable);

    metadata.writeMeta(os);

    // Write the number of non-null grids.
    int32_t gridCount = 0;
    for (GridCPtrVecCIter i = grids.begin(), e = grids.end(); i != e; ++i) {
        if (*i) ++gridCount;
    }
    os.write(reinterpret_cast<char*>(&gridCount), sizeof(int32_t));

    using TreeMap = std::map<const TreeBase*, GridDescriptor>;
    using TreeMapIter = TreeMap::iterator;
    TreeMap treeMap;

    // Determine which grid names are unique and which are not.
    using NameHistogram = std::map<std::string, int /*count*/>;
    NameHistogram nameCount;
    for (GridCPtrVecCIter i = grids.begin(), e = grids.end(); i != e; ++i) {
        if (const GridBase::ConstPtr& grid = *i) {
            const std::string name = grid->getName();
            NameHistogram::iterator it = nameCount.find(name);
            if (it != nameCount.end()) it->second++;
            else nameCount[name] = 1;
        }
    }

    std::set<std::string> uniqueNames;

    // Write out the non-null grids.
    for (GridCPtrVecCIter i = grids.begin(), e = grids.end(); i != e; ++i) {
        if (const GridBase::ConstPtr& grid = *i) {

            // Ensure that the grid's descriptor has a unique grid name, by appending
            // a number to it if a grid with the same name was already written.
            // Always add a number if the grid name is empty, so that the grid can be
            // properly identified as an instance parent, if necessary.
            std::string name = grid->getName();
            if (name.empty() || nameCount[name] > 1) {
                name = GridDescriptor::addSuffix(name, 0);
            }
            for (int n = 1; uniqueNames.find(name) != uniqueNames.end(); ++n) {
                name = GridDescriptor::addSuffix(grid->getName(), n);
            }
            uniqueNames.insert(name);

            // Create a grid descriptor.
            GridDescriptor gd(name, grid->type(), grid->saveFloatAsHalf());

            // Check if this grid's tree is shared with a grid that has already been written.
            const TreeBase* treePtr = &(grid->baseTree());
            TreeMapIter mapIter = treeMap.find(treePtr);

            bool isInstance = ((mapIter != treeMap.end())
                && (mapIter->second.saveFloatAsHalf() == gd.saveFloatAsHalf()));

            if (mEnableInstancing && isInstance) {
                // This grid's tree is shared with another grid that has already been written.
                // Get the name of the other grid.
                gd.setInstanceParentName(mapIter->second.uniqueName());
                // Write out this grid's descriptor and metadata, but not its tree.
                writeGridInstance(gd, grid, os, seekable);

                OPENVDB_LOG_DEBUG_RUNTIME("io::Archive::write(): "
                    << GridDescriptor::nameAsString(gd.uniqueName())
                    << " (" << std::hex << treePtr << std::dec << ")"
                    << " is an instance of "
                    << GridDescriptor::nameAsString(gd.instanceParentName()));
            } else {
                // Write out the grid descriptor and its associated grid.
                writeGrid(gd, grid, os, seekable);
                // Record the grid's tree pointer so that the tree doesn't get written
                // more than once.
                treeMap[treePtr] = gd;
            }
        }

        // Some compression options (e.g., mask compression) are set per grid.
        // Restore the original settings before writing the next grid.
        io::setDataCompression(os, compression());
    }
}


void
Archive::writeGrid(GridDescriptor& gd, GridBase::ConstPtr grid,
    std::ostream& os, bool seekable) const
{
    // Restore file-level stream metadata on exit.
    struct OnExit {
        OnExit(std::ios_base& strm_): strm(&strm_), ptr(strm_.pword(GetSteamState().metadata)) {}
        ~OnExit() { strm->pword(GetSteamState().metadata) = ptr; }
        std::ios_base* strm;
        void* ptr;
    };
    OnExit restore(os);

    // Stream metadata varies per grid, so make a copy of the file-level stream metadata.
    io::StreamMetadata::Ptr streamMetadata;
    if (io::StreamMetadata::Ptr meta = io::getStreamMetadataPtr(os)) {
        streamMetadata.reset(new StreamMetadata(*meta));
    } else {
        streamMetadata.reset(new StreamMetadata);
    }
    streamMetadata->setHalfFloat(grid->saveFloatAsHalf());
    streamMetadata->gridMetadata() = static_cast<const MetaMap&>(*grid);
    io::setStreamMetadataPtr(os, streamMetadata, /*transfer=*/false);

    // Write out the Descriptor's header information (grid name and type)
    gd.writeHeader(os);

    // Save the curent stream position as postion to where the offsets for
    // this GridDescriptor will be written to.
    int64_t offsetPos = (seekable ? int64_t(os.tellp()) : 0);

    // Write out the offset information. At this point it will be incorrect.
    // But we need to write it out to move the stream head forward.
    gd.writeStreamPos(os);

    // Now we know the starting grid storage position.
    if (seekable) gd.setGridPos(os.tellp());

    // Save the compression settings for this grid.
    setGridCompression(os, *grid);

    // copy grid and add delay load metadata
    const auto copyOfGrid = grid->copyGrid(); // shallow copy
    const auto nonConstCopyOfGrid = ConstPtrCast<GridBase>(copyOfGrid);
    nonConstCopyOfGrid->insertMeta(GridBase::META_FILE_DELAYED_LOAD,
        DelayedLoadMetadata());
    DelayedLoadMetadata::Ptr delayLoadMeta =
        nonConstCopyOfGrid->getMetadata<DelayedLoadMetadata>(GridBase::META_FILE_DELAYED_LOAD);
    if (!populateDelayedLoadMetadata(*delayLoadMeta, *grid, compression())) {
        nonConstCopyOfGrid->removeMeta(GridBase::META_FILE_DELAYED_LOAD);
    }

    // Save the grid's metadata and transform.
    if (getWriteGridStatsMetadata(os)) {
        // Compute and add grid statistics metadata.
        nonConstCopyOfGrid->addStatsMetadata();
        nonConstCopyOfGrid->insertMeta(GridBase::META_FILE_COMPRESSION,
            StringMetadata(compressionToString(getDataCompression(os))));
    }
    copyOfGrid->writeMeta(os);
    grid->writeTransform(os);

    // Save the grid's structure.
    grid->writeTopology(os);

    // Now we know the grid block storage position.
    if (seekable) gd.setBlockPos(os.tellp());

    // Save out the data blocks of the grid.
    grid->writeBuffers(os);

    // Now we know the end position of this grid.
    if (seekable) gd.setEndPos(os.tellp());

    if (seekable) {
        // Now, go back to where the Descriptor's offset information is written
        // and write the offsets again.
        os.seekp(offsetPos, std::ios_base::beg);
        gd.writeStreamPos(os);

        // Now seek back to the end.
        gd.seekToEnd(os);
    }
}


void
Archive::writeGridInstance(GridDescriptor& gd, GridBase::ConstPtr grid,
    std::ostream& os, bool seekable) const
{
    // Write out the Descriptor's header information (grid name, type
    // and instance parent name).
    gd.writeHeader(os);

    // Save the curent stream position as postion to where the offsets for
    // this GridDescriptor will be written to.
    int64_t offsetPos = (seekable ? int64_t(os.tellp()) : 0);

    // Write out the offset information. At this point it will be incorrect.
    // But we need to write it out to move the stream head forward.
    gd.writeStreamPos(os);

    // Now we know the starting grid storage position.
    if (seekable) gd.setGridPos(os.tellp());

    // Save the compression settings for this grid.
    setGridCompression(os, *grid);

    // Save the grid's metadata and transform.
    grid->writeMeta(os);
    grid->writeTransform(os);

    // Now we know the end position of this grid.
    if (seekable) gd.setEndPos(os.tellp());

    if (seekable) {
        // Now, go back to where the Descriptor's offset information is written
        // and write the offsets again.
        os.seekp(offsetPos, std::ios_base::beg);
        gd.writeStreamPos(os);

        // Now seek back to the end.
        gd.seekToEnd(os);
    }
}

} // namespace io
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb
