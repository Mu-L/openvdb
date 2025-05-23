// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0

#include "Grid.h"
#include "Metadata.h"
#include "util/Name.h"
#include <mutex>


namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {

/// @note For Houdini compatibility, boolean-valued metadata names
/// should begin with "is_".
const char
    * const GridBase::META_GRID_CLASS = "class",
    * const GridBase::META_GRID_CREATOR = "creator",
    * const GridBase::META_GRID_NAME = "name",
    * const GridBase::META_SAVE_HALF_FLOAT = "is_saved_as_half_float",
    * const GridBase::META_IS_LOCAL_SPACE = "is_local_space",
    * const GridBase::META_VECTOR_TYPE = "vector_type",
    * const GridBase::META_FILE_BBOX_MIN = "file_bbox_min",
    * const GridBase::META_FILE_BBOX_MAX = "file_bbox_max",
    * const GridBase::META_FILE_COMPRESSION = "file_compression",
    * const GridBase::META_FILE_MEM_BYTES = "file_mem_bytes",
    * const GridBase::META_FILE_VOXEL_COUNT = "file_voxel_count",
    * const GridBase::META_FILE_DELAYED_LOAD = "file_delayed_load";


////////////////////////////////////////


namespace {

using GridFactoryMap = std::map<Name, GridBase::GridFactory>;
using GridFactoryMapCIter = GridFactoryMap::const_iterator;

struct LockedGridRegistry {
    LockedGridRegistry() {}
    std::mutex mMutex;
    GridFactoryMap mMap;
};


// Global function for accessing the registry
LockedGridRegistry*
getGridRegistry()
{
    static LockedGridRegistry registry;
    return &registry;
}

} // unnamed namespace


bool
GridBase::isRegistered(const Name& name)
{
    LockedGridRegistry* registry = getGridRegistry();
    std::lock_guard<std::mutex> lock(registry->mMutex);

    return (registry->mMap.find(name) != registry->mMap.end());
}


void
GridBase::registerGrid(const Name& name, GridFactory factory)
{
    LockedGridRegistry* registry = getGridRegistry();
    std::lock_guard<std::mutex> lock(registry->mMutex);

    if (registry->mMap.find(name) != registry->mMap.end()) {
        OPENVDB_THROW(KeyError, "Grid type " << name << " is already registered");
    }

    registry->mMap[name] = factory;
}


void
GridBase::unregisterGrid(const Name& name)
{
    LockedGridRegistry* registry = getGridRegistry();
    std::lock_guard<std::mutex> lock(registry->mMutex);

    registry->mMap.erase(name);
}


GridBase::Ptr
GridBase::createGrid(const Name& name)
{
    LockedGridRegistry* registry = getGridRegistry();
    std::lock_guard<std::mutex> lock(registry->mMutex);

    GridFactoryMapCIter iter = registry->mMap.find(name);

    if (iter == registry->mMap.end()) {
        OPENVDB_THROW(LookupError, "Cannot create grid of unregistered type " << name);
    }

    return (iter->second)();
}


void
GridBase::clearRegistry()
{
    LockedGridRegistry* registry = getGridRegistry();
    std::lock_guard<std::mutex> lock(registry->mMutex);

    registry->mMap.clear();
}


////////////////////////////////////////


GridClass
GridBase::stringToGridClass(const std::string& s)
{
    GridClass ret = GRID_UNKNOWN;
    std::string str = s;
    openvdb::string::trim(str);
    openvdb::string::to_lower(str);
    if (str == gridClassToString(GRID_LEVEL_SET)) {
        ret = GRID_LEVEL_SET;
    } else if (str == gridClassToString(GRID_FOG_VOLUME)) {
        ret = GRID_FOG_VOLUME;
    } else if (str == gridClassToString(GRID_STAGGERED)) {
        ret = GRID_STAGGERED;
    }
    return ret;
}


std::string
GridBase::gridClassToString(GridClass cls)
{
    std::string ret;
    switch (cls) {
        case GRID_UNKNOWN: ret = "unknown"; break;
        case GRID_LEVEL_SET: ret = "level set"; break;
        case GRID_FOG_VOLUME: ret = "fog volume"; break;
        case GRID_STAGGERED: ret = "staggered"; break;
    }
    return ret;
}

std::string
GridBase::gridClassToMenuName(GridClass cls)
{
    std::string ret;
    switch (cls) {
        case GRID_UNKNOWN: ret = "Other"; break;
        case GRID_LEVEL_SET: ret = "Level Set"; break;
        case GRID_FOG_VOLUME: ret = "Fog Volume"; break;
        case GRID_STAGGERED: ret = "Staggered Vector Field"; break;
    }
    return ret;
}



GridClass
GridBase::getGridClass() const
{
    GridClass cls = GRID_UNKNOWN;
    if (StringMetadata::ConstPtr s = this->getMetadata<StringMetadata>(META_GRID_CLASS)) {
        cls = stringToGridClass(s->value());
    }
    return cls;
}


void
GridBase::setGridClass(GridClass cls)
{
    this->insertMeta(META_GRID_CLASS, StringMetadata(gridClassToString(cls)));
}


void
GridBase::clearGridClass()
{
    this->removeMeta(META_GRID_CLASS);
}


////////////////////////////////////////


VecType
GridBase::stringToVecType(const std::string& s)
{
    VecType ret = VEC_INVARIANT;
    std::string str = s;
    openvdb::string::trim(str);
    openvdb::string::to_lower(str);
    if (str == vecTypeToString(VEC_COVARIANT)) {
        ret = VEC_COVARIANT;
    } else if (str == vecTypeToString(VEC_COVARIANT_NORMALIZE)) {
        ret = VEC_COVARIANT_NORMALIZE;
    } else if (str == vecTypeToString(VEC_CONTRAVARIANT_RELATIVE)) {
        ret = VEC_CONTRAVARIANT_RELATIVE;
    } else if (str == vecTypeToString(VEC_CONTRAVARIANT_ABSOLUTE)) {
        ret = VEC_CONTRAVARIANT_ABSOLUTE;
    }
    return ret;
}


std::string
GridBase::vecTypeToString(VecType typ)
{
    std::string ret;
    switch (typ) {
        case VEC_INVARIANT: ret = "invariant"; break;
        case VEC_COVARIANT: ret = "covariant"; break;
        case VEC_COVARIANT_NORMALIZE: ret = "covariant normalize"; break;
        case VEC_CONTRAVARIANT_RELATIVE: ret = "contravariant relative"; break;
        case VEC_CONTRAVARIANT_ABSOLUTE: ret = "contravariant absolute"; break;
    }
    return ret;
}


std::string
GridBase::vecTypeExamples(VecType typ)
{
    std::string ret;
    switch (typ) {
        case VEC_INVARIANT: ret = "Tuple/Color/UVW"; break;
        case VEC_COVARIANT: ret = "Gradient/Normal"; break;
        case VEC_COVARIANT_NORMALIZE: ret = "Unit Normal"; break;
        case VEC_CONTRAVARIANT_RELATIVE: ret = "Displacement/Velocity/Acceleration"; break;
        case VEC_CONTRAVARIANT_ABSOLUTE: ret = "Position"; break;
    }
    return ret;
}


std::string
GridBase::vecTypeDescription(VecType typ)
{
    std::string ret;
    switch (typ) {
        case VEC_INVARIANT:
            ret = "Does not transform";
            break;
        case VEC_COVARIANT:
            ret = "Apply the inverse-transpose transform matrix but ignore translation";
            break;
        case VEC_COVARIANT_NORMALIZE:
            ret = "Apply the inverse-transpose transform matrix but ignore translation"
                " and renormalize vectors";
            break;
        case VEC_CONTRAVARIANT_RELATIVE:
            ret = "Apply the forward transform matrix but ignore translation";
            break;
        case VEC_CONTRAVARIANT_ABSOLUTE:
            ret = "Apply the forward transform matrix, including translation";
            break;
    }
    return ret;
}


VecType
GridBase::getVectorType() const
{
    VecType typ = VEC_INVARIANT;
    if (StringMetadata::ConstPtr s = this->getMetadata<StringMetadata>(META_VECTOR_TYPE)) {
        typ = stringToVecType(s->value());
    }
    return typ;
}


void
GridBase::setVectorType(VecType typ)
{
    this->insertMeta(META_VECTOR_TYPE, StringMetadata(vecTypeToString(typ)));
}


void
GridBase::clearVectorType()
{
    this->removeMeta(META_VECTOR_TYPE);
}


////////////////////////////////////////


std::string
GridBase::getName() const
{
    if (Metadata::ConstPtr meta = (*this)[META_GRID_NAME]) return meta->str();
    return "";
}


void
GridBase::setName(const std::string& name)
{
    this->removeMeta(META_GRID_NAME);
    this->insertMeta(META_GRID_NAME, StringMetadata(name));
}


////////////////////////////////////////


std::string
GridBase::getCreator() const
{
    if (Metadata::ConstPtr meta = (*this)[META_GRID_CREATOR]) return meta->str();
    return "";
}


void
GridBase::setCreator(const std::string& creator)
{
    this->removeMeta(META_GRID_CREATOR);
    this->insertMeta(META_GRID_CREATOR, StringMetadata(creator));
}


////////////////////////////////////////


bool
GridBase::saveFloatAsHalf() const
{
    if (Metadata::ConstPtr meta = (*this)[META_SAVE_HALF_FLOAT]) {
        return meta->asBool();
    }
    return false;
}


void
GridBase::setSaveFloatAsHalf(bool saveAsHalf)
{
    this->removeMeta(META_SAVE_HALF_FLOAT);
    this->insertMeta(META_SAVE_HALF_FLOAT, BoolMetadata(saveAsHalf));
}


////////////////////////////////////////


bool
GridBase::isInWorldSpace() const
{
    bool local = false;
    if (Metadata::ConstPtr meta = (*this)[META_IS_LOCAL_SPACE]) {
        local = meta->asBool();
    }
    return !local;
}


void
GridBase::setIsInWorldSpace(bool world)
{
    this->removeMeta(META_IS_LOCAL_SPACE);
    this->insertMeta(META_IS_LOCAL_SPACE, BoolMetadata(!world));
}


////////////////////////////////////////


void
GridBase::addStatsMetadata()
{
    const CoordBBox bbox = this->evalActiveVoxelBoundingBox();
    this->removeMeta(META_FILE_BBOX_MIN);
    this->removeMeta(META_FILE_BBOX_MAX);
    this->removeMeta(META_FILE_MEM_BYTES);
    this->removeMeta(META_FILE_VOXEL_COUNT);
    this->insertMeta(META_FILE_BBOX_MIN,    Vec3IMetadata(bbox.min().asVec3i()));
    this->insertMeta(META_FILE_BBOX_MAX,    Vec3IMetadata(bbox.max().asVec3i()));
    this->insertMeta(META_FILE_MEM_BYTES,   Int64Metadata(this->memUsage()));
    this->insertMeta(META_FILE_VOXEL_COUNT, Int64Metadata(this->activeVoxelCount()));
}


MetaMap::Ptr
GridBase::getStatsMetadata() const
{
    const char* const fields[] = {
        META_FILE_BBOX_MIN,
        META_FILE_BBOX_MAX,
        META_FILE_MEM_BYTES,
        META_FILE_VOXEL_COUNT,
        nullptr
    };

    /// @todo Check that the fields are of the correct type?
    MetaMap::Ptr ret(new MetaMap);
    for (int i = 0; fields[i] != nullptr; ++i) {
        if (Metadata::ConstPtr m = (*this)[fields[i]]) {
            ret->insertMeta(fields[i], *m);
        }
    }
    return ret;
}


////////////////////////////////////////


void
GridBase::clipGrid(const BBoxd& worldBBox)
{
    const CoordBBox indexBBox =
        this->constTransform().worldToIndexNodeCentered(worldBBox);
    this->clip(indexBBox);
}

} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb
