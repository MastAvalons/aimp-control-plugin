// Copyright (c) 2011, Alexey Ivanov

#include "stdafx.h"
#include "manager3_impl.h"
#include "manager_impl_common.h"
#include "playlist_entry.h"
#include "aimp3_sdk/aimp3_sdk.h"
#include "utils/iunknown_impl.h"
#include "plugin/logger.h"
#include "utils/string_encoding.h"
#include "utils/image.h"
#include "utils/util.h"
#include <boost/assign/std.hpp>
#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace {
using namespace ControlPlugin::PluginLogger;
ModuleLoggerType& logger()
    { return getLogManager().getModuleLogger<AIMPPlayer::AIMPManager>(); }
}


/* Specialization of Utilities::crc32(T) for AIMP3SDK::TAIMPFileInfo struct.
     Be sure that following assertion is ok:
        TAIMPFileInfo info;
        PlaylistEntry entry(info);
        assert( crc32(entry) == crc32(info) )
*/
template<>
crc32_t Utilities::crc32<AIMP3SDK::TAIMPFileInfo>(const AIMP3SDK::TAIMPFileInfo& info)
{
#define ENTRY_FIELD_CRC32(field) (info.field##Length == 0) ? 0 : Utilities::crc32( &info.field[0], info.field##Length * sizeof(info.field[0]) )
    const crc32_t members_crc32_list [] = {
        ENTRY_FIELD_CRC32(Album),
        ENTRY_FIELD_CRC32(Artist),
        ENTRY_FIELD_CRC32(Date),
        ENTRY_FIELD_CRC32(FileName),
        ENTRY_FIELD_CRC32(Genre),
        ENTRY_FIELD_CRC32(Title),
        info.BitRate,
        info.Channels,
        info.Duration,
        Utilities::crc32(info.FileSize),
        info.Rating,
        info.SampleRate
    };
#undef ENTRY_FIELD_CRC32

    return Utilities::crc32( &members_crc32_list[0], sizeof(members_crc32_list) );
}


namespace AIMPPlayer
{

using namespace Utilities;

const AIMP3SDK::HPLS kInvalidPlaylistId = nullptr;
const int kNoParam1 = 0;
void * const kNoParam2 = nullptr;

//! general tempate for convinient casting. Provide specialization for your own types.
template<typename To, typename From> To cast(From);

template<>
PlaylistID cast(AIMP3SDK::HPLS handle)
{
    return reinterpret_cast<PlaylistID>(handle);
}

template<>
AIMP3SDK::HPLS cast(PlaylistID id)
{
    return reinterpret_cast<AIMP3SDK::HPLS>(id);
}

//template<>
//PlaylistEntryID cast(AIMP3SDK::HPLSENTRY handle)
//{
//    return reinterpret_cast<PlaylistEntryID>(handle);
//}
//
//template<>
//AIMP3SDK::HPLSENTRY cast(PlaylistEntryID id)
//{
//    return reinterpret_cast<AIMP3SDK::HPLSENTRY>(id);
//}

class AIMPCoreUnitMessageHook : public IUnknownInterfaceImpl<AIMP3SDK::IAIMPCoreUnitMessageHook>
{
public:
    AIMPCoreUnitMessageHook(AIMP3Manager* aimp3_manager)
        : 
        aimp3_manager_(aimp3_manager)
    {}

    virtual void WINAPI CoreMessage(DWORD AMessage, int AParam1, void *AParam2, HRESULT *AResult) {
        aimp3_manager_->OnAimpCoreMessage(AMessage, AParam1, AParam2, AResult);
    }

private:

    AIMP3Manager* aimp3_manager_;
};

AIMP3Manager::AIMP3Manager(boost::intrusive_ptr<AIMP3SDK::IAIMPCoreUnit> aimp3_core_unit)
    :
    aimp3_core_unit_(aimp3_core_unit),
    next_listener_id_(0)
{
    try {
        initializeAIMPObjects();
    } catch (std::runtime_error& e) {
        throw std::runtime_error( std::string("Error occured while AIMP3Manager initialization. Reason:") + e.what() );
    }
    ///!!!register listeners here
    aimp3_core_message_hook_.reset( new AIMPCoreUnitMessageHook(this) );
    aimp3_core_unit_->MessageHook( aimp3_core_message_hook_.get() );
}

AIMP3Manager::~AIMP3Manager()
{
    ///!!!unregister listeners here
    aimp3_core_unit_->MessageUnhook( aimp3_core_message_hook_.get() );
    aimp3_core_message_hook_.reset();
}

void AIMP3Manager::initializeAIMPObjects()
{
    using namespace AIMP3SDK;
    IAIMPAddonsPlayerManager* player_manager;
    if (S_OK != aimp3_core_unit_->QueryInterface(IID_IAIMPAddonsPlayerManager, 
                                                 reinterpret_cast<void**>(&player_manager)
                                                 ) 
        )
    {
        throw std::runtime_error("Creation object IAIMPAddonsPlayerManager failed"); 
    }
    aimp3_player_manager_.reset(player_manager);

    IAIMPAddonsPlaylistManager* playlist_manager;
    if (S_OK != aimp3_core_unit_->QueryInterface(IID_IAIMPAddonsPlaylistManager, 
                                                 reinterpret_cast<void**>(&playlist_manager)
                                                 ) 
        )
    {
        throw std::runtime_error("Creation object IAIMPAddonsPlaylistManager failed"); 
    }
    aimp3_playlist_manager_.reset(playlist_manager);

    IAIMPAddonsCoverArtManager* coverart_manager;
    if (S_OK != aimp3_core_unit_->QueryInterface(IID_IAIMPAddonsCoverArtManager, 
                                                 reinterpret_cast<void**>(&coverart_manager)
                                                 ) 
        )
    {
        throw std::runtime_error("Creation object IAIMPAddonsCoverArtManager failed"); 
    }
    aimp3_coverart_manager_.reset(coverart_manager);
}

Playlist AIMP3Manager::loadPlaylist(int playlist_index)
{
    const char * const error_prefix = "Error occured while extracting playlist data: ";
    
    AIMP3SDK::HPLS id = aimp3_playlist_manager_->StorageGet(playlist_index);
    if (id == kInvalidPlaylistId) {
        throw std::runtime_error(MakeString() << error_prefix << "IAIMPAddonsPlaylistManager::StorageGet failed");
    }

    using namespace AIMP3SDK;
    
    HRESULT r;
    INT64 duration;
    r = aimp3_playlist_manager_->StoragePropertyGetValue( id, AIMP_PLAYLIST_STORAGE_PROPERTY_DURATION, &duration, sizeof(duration) );
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << error_prefix << "IAIMPAddonsPlaylistManager::StoragePropertyGetValue(AIMP_PLAYLIST_STORAGE_PROPERTY_DURATION) failed. Result " << r);
    }
    INT64 size;
    r = aimp3_playlist_manager_->StoragePropertyGetValue( id, AIMP_PLAYLIST_STORAGE_PROPERTY_SIZE, &size, sizeof(size) );
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << error_prefix << "IAIMPAddonsPlaylistManager::StoragePropertyGetValue(AIMP_PLAYLIST_STORAGE_PROPERTY_SIZE) failed. Result " << r);
    }

    const size_t name_length = 256;
    WCHAR name[name_length + 1] = {0};
    r = aimp3_playlist_manager_->StoragePropertyGetValue(id, AIMP_PLAYLIST_STORAGE_PROPERTY_NAME, name, name_length);
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << error_prefix << "IAIMPAddonsPlaylistManager::StoragePropertyGetValue(AIMP_PLAYLIST_STORAGE_PROPERTY_NAME) failed. Result " << r);
    }

    const int entries_count = aimp3_playlist_manager_->StorageGetEntryCount(id);

    return Playlist(name,
                    entries_count,
                    duration,
                    size,
                    cast<PlaylistID>(id)
                    );
}

/*
    Helper for convinient load entry data from AIMP.
    Prepares AIMP3SDK::TAIMPFileInfo struct.
    Contains necessary string buffers which are always in clear state.
*/
class AIMP3FileInfoHelper
{
public:

    AIMP3SDK::TAIMPFileInfo& getEmptyFileInfo()
    {
        memset( &info_, 0, sizeof(info_) );
        info_.StructSize = sizeof(info_);
        // clear all buffers content
        WCHAR* field_buffers[] = { album, artist, date, filename, genre, title };
        BOOST_FOREACH(WCHAR* field_buffer, field_buffers) {
            memset( field_buffer, 0, kFIELDBUFFERSIZE * sizeof(field_buffer[0]) );
        }
        // set buffers length
        info_.AlbumLength = info_.ArtistLength = info_.DateLength
        = info_.DateLength = info_.FileNameLength = info_.GenreLength = info_.TitleLength
        = kFIELDBUFFERSIZE;
        // set buffers
        info_.Album = album;
        info_.Artist = artist;
        info_.Date = date;
        info_.FileName = filename;
        info_.Genre = genre;
        info_.Title = title;

        return info_;
    }

    AIMP3SDK::TAIMPFileInfo& getFileInfo()
        { return info_; }

    PlaylistEntry getPlaylistEntry(DWORD entry_id, crc32_t crc32 = 0)
    {
        getFileInfoWithCorrectStringLengths();

        return PlaylistEntry(   info_.Album,    info_.AlbumLength,
                                info_.Artist,   info_.ArtistLength,
                                info_.Date,     info_.DateLength,
                                info_.FileName, info_.FileNameLength,
                                info_.Genre,    info_.GenreLength,
                                info_.Title,    info_.TitleLength,
                                // info_.Active - useless, not used.
                                info_.BitRate,
                                info_.Channels,
                                info_.Duration,
                                info_.FileSize,
                                info_.Rating,
                                info_.SampleRate,
                                info_.TrackNumber,
                                entry_id,
                                crc32
                            );
    }

    /* \return track ID. */
    DWORD getTrackNumber() const
        { return info_.TrackNumber; }

    AIMP3SDK::TAIMPFileInfo& getFileInfoWithCorrectStringLengths()
    {
        // fill string lengths if Aimp do not do this.
        if (info_.AlbumLength == kFIELDBUFFERSIZE) {
            info_.AlbumLength = std::wcslen(info_.Album);
        }

        if (info_.ArtistLength == kFIELDBUFFERSIZE) {
            info_.ArtistLength = std::wcslen(info_.Artist);
        }

        if (info_.DateLength == kFIELDBUFFERSIZE) {
            info_.DateLength = std::wcslen(info_.Date);
        }

        if (info_.FileNameLength == kFIELDBUFFERSIZE) {
            info_.FileNameLength = std::wcslen(info_.FileName);
        }

        if (info_.GenreLength == kFIELDBUFFERSIZE) {
            info_.GenreLength = std::wcslen(info_.Genre);
        }

        if (info_.TitleLength == kFIELDBUFFERSIZE) {
            info_.TitleLength = std::wcslen(info_.Title);
        }

        return info_;
    }

private:

    AIMP3SDK::TAIMPFileInfo info_;
    static const DWORD kFIELDBUFFERSIZE = MAX_PATH;
    WCHAR album[kFIELDBUFFERSIZE + 1];
    WCHAR artist[kFIELDBUFFERSIZE + 1];
    WCHAR date[kFIELDBUFFERSIZE + 1];
    WCHAR filename[kFIELDBUFFERSIZE + 1];
    WCHAR genre[kFIELDBUFFERSIZE + 1];
    WCHAR title[kFIELDBUFFERSIZE + 1];
};

void AIMP3Manager::loadEntries(Playlist& playlist) // throws std::runtime_error
{
    using namespace AIMP3SDK;
    // PROFILE_EXECUTION_TIME(__FUNCTION__);

    HRESULT r;

    const AIMP3SDK::HPLS playlist_id = cast<AIMP3SDK::HPLS>( playlist.getID() );    

    boost::intrusive_ptr<IAIMPAddonsPlaylistStrings> strings;
    {
        IAIMPAddonsPlaylistStrings* strings_raw = nullptr;
        r = aimp3_playlist_manager_->StorageGetFiles(playlist_id, 0, &strings_raw);
        if (S_OK != r) {
            throw std::runtime_error(MakeString() << "IAIMPAddonsPlaylistManager::StorageGetFiles(" << playlist_id << ") failed. Result " << r);
        }
        strings.reset(strings_raw);
    }

    const int entries_count = strings->ItemGetCount();

    AIMP3FileInfoHelper file_info_helper; // used for get entries from AIMP conviniently.

    // temp objects to prevent partial change state of passed objects when error occurs.
    EntriesListType entries;
    entries.reserve(entries_count);
    
    for (int entry_index = 0; entry_index < entries_count; ++entry_index) {
        r = strings->ItemGetInfo( entry_index, &file_info_helper.getEmptyFileInfo() );
        if (S_OK == r) {
            entries.push_back( file_info_helper.getPlaylistEntry(entry_index) ); ///!!! Maybe we need to use this instead index: HPLSENTRY entry_id = aimp3_playlist_manager_->StorageGetEntry(playlist_id, entry_index);
        } else {
            BOOST_LOG_SEV(logger(), error) << "Error " << r << " occured while getting entry info �" << entry_index << " from playlist with ID = " << playlist_id;
            throw std::runtime_error("Error occured while getting playlist entries.");
        }
    }

    // we got list, save result
    playlist.getEntries().swap(entries);
}

void AIMP3Manager::startPlayback()
{
    // play current track.
    //aimp3_player_manager_->PlayStorage(aimp3_playlist_manager_->StoragePlayingGet(), -1); //aimp2_player_->PlayOrResume();
    using namespace AIMP3SDK;
    aimp3_core_unit_->MessageSend(AIMP_MSG_CMD_PLAY, kNoParam1, kNoParam2);
}

void AIMP3Manager::startPlayback(TrackDescription track_desc) // throws std::runtime_error
{
    HRESULT r = aimp3_player_manager_->PlayStorage(cast<AIMP3SDK::HPLS>(track_desc.playlist_id), track_desc.track_id);
    if (S_OK != r) {
        throw std::runtime_error( MakeString() << "Error " << r << " in "__FUNCTION__" with " << track_desc );
    }
}

void AIMP3Manager::stopPlayback()
{
    using namespace AIMP3SDK;
    aimp3_core_unit_->MessageSend(AIMP_MSG_CMD_STOP, kNoParam1, kNoParam2); // aimp2_player_->Stop();
}

void AIMP3Manager::pausePlayback()
{
    using namespace AIMP3SDK;
    aimp3_core_unit_->MessageSend(AIMP_MSG_CMD_PLAYPAUSE, kNoParam1, kNoParam2); // aimp2_player_->Pause();
}

void AIMP3Manager::playNextTrack()
{
    using namespace AIMP3SDK;
    aimp3_core_unit_->MessageSend(AIMP_MSG_CMD_NEXT, kNoParam1, kNoParam2); // aimp2_player_->NextTrack();
}

void AIMP3Manager::playPreviousTrack()
{
    using namespace AIMP3SDK;
    aimp3_core_unit_->MessageSend(AIMP_MSG_CMD_PREV, kNoParam1, kNoParam2); // aimp2_player_->PrevTrack();
}

void AIMP3Manager::OnAimpCoreMessage(DWORD AMessage, int AParam1, void* AParam2, HRESULT* AResult)
{
    assert(AResult);
    *AResult = E_FAIL;
}

//void AIMP3Manager::notifyAboutInternalEventOnStatusChange(AIMP3Manager::STATUS status)
//{
//    switch (status) {
//    case STATUS_SHUFFLE:
//        notifyAboutInternalEvent(SHUFFLE_EVENT);
//        break;
//    case STATUS_REPEAT:
//        notifyAboutInternalEvent(REPEAT_EVENT);
//        break;
//    case STATUS_VOLUME:
//        notifyAboutInternalEvent(VOLUME_EVENT);
//        break;
//    case STATUS_MUTE:
//        notifyAboutInternalEvent(MUTE_EVENT);
//        break;
//    case STATUS_POS:
//        notifyAboutInternalEvent(TRACK_PROGRESS_CHANGED_DIRECTLY_EVENT);
//        break;
//    default:
//        // do nothing, about other status changes AIMP will notify us itself.
//        break;
//    }
//}

void AIMP3Manager::setStatus(AIMPManager::STATUS status, AIMPManager::StatusValue value)
{
    //try {
    //    if ( FALSE == aimp2_controller_->AIMP_Status_Set(cast<AIMP2SDK_STATUS>(status), value) ) {
    //        throw std::runtime_error(MakeString() << "Error occured while setting status " << asString(status) << " to value " << value);
    //    }
    //} catch (std::bad_cast& e) {
    //    throw std::runtime_error( e.what() );
    //}

    //notifyAboutInternalEventOnStatusChange(status);
}

AIMP3Manager::StatusValue AIMP3Manager::getStatus(AIMP3Manager::STATUS status) const
{
    //return aimp2_controller_->AIMP_Status_Get(status);
    return 0;
}

std::string AIMP3Manager::getAIMPVersion() const
{
    using namespace AIMP3SDK;
    TAIMPVersionInfo version_info = {0};
    HRESULT r = aimp3_core_unit_->GetVersion(&version_info);

    if (S_OK != r) {
        BOOST_LOG_SEV(logger(), error) << "IAIMPCoreUnit::GetVersion returned " << r;
        return "";
    }
    
    const int version = version_info.BuildNumber;
    using namespace std;
    ostringstream os;
    os << version / 1000 << '.' << setfill('0') << setw(2) << (version % 1000) / 10 << '.' << version % 10
       << ' ' << (version_info.BuildSuffix) ? StringEncoding::utf16_to_system_ansi_encoding_safe(version_info.BuildSuffix) : "";
    return os.str();
}

PlaylistID AIMP3Manager::getActivePlaylist() const
{
    // return AIMP internal playlist ID here since AIMP3Manager uses the same IDs.
    return cast<PlaylistID>( aimp3_playlist_manager_->StoragePlayingGet() );
}

PlaylistEntryID AIMP3Manager::getActiveEntry() const
{
    using namespace AIMP3SDK;
    const PlaylistID active_playlist = getActivePlaylist();
    int internal_active_entry_index;
    aimp3_playlist_manager_->StoragePropertyGetValue( cast<AIMP3SDK::HPLS>(active_playlist), AIMP_PLAYLIST_STORAGE_PROPERTY_TRACKINGINDEX,
                                                      &internal_active_entry_index, sizeof(internal_active_entry_index) 
                                                     );

    // internal index equals AIMP3Manager's entry ID. In other case map index<->ID(use Playlist::entries_id_list_) here in all places where TrackDescription is used.
    const PlaylistEntryID entry_id = internal_active_entry_index;
    return entry_id;
}

TrackDescription AIMP3Manager::getActiveTrack() const
{
    return TrackDescription( getActivePlaylist(), getActiveEntry() );
}

AIMP3Manager::PLAYBACK_STATE AIMP3Manager::getPlaybackState() const
{
    PLAYBACK_STATE state = STOPPED;
    StatusValue internal_state = getStatus(STATUS_Player);
    // map internal AIMP state to PLAYBACK_STATE.
    switch (internal_state) {
    case 0:
        state = STOPPED;
        break;
    case 1:
        state = PLAYING;
        break;
    case 2:
        state = PAUSED;
        break;
    default:
        assert(!"getStatus(STATUS_Player) returned unknown value");
        BOOST_LOG_SEV(logger(), error) << "getStatus(STATUS_Player) returned unknown value " << internal_state;
    }

    return state;
}

AIMP3SDK::HPLSENTRY getEntryHandle(TrackDescription track_desc,
                                   boost::intrusive_ptr<AIMP3SDK::IAIMPAddonsPlaylistManager> aimp3_playlist_manager)
{
    // Note: TrackDescription's track_id is really entry index, not HPLSENTRY value.
    return aimp3_playlist_manager->StorageGetEntry(cast<AIMP3SDK::HPLS>(track_desc.playlist_id), track_desc.track_id);    
}

void AIMP3Manager::enqueueEntryForPlay(TrackDescription track_desc, bool insert_at_queue_beginning) // throws std::runtime_error
{
    using namespace AIMP3SDK;
    AIMP3SDK::HPLSENTRY entry_handle = getEntryHandle(track_desc, aimp3_playlist_manager_);
    HRESULT r = aimp3_playlist_manager_->QueueEntryAdd(entry_handle, insert_at_queue_beginning);
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << "Error " << r << " in "__FUNCTION__" with " << track_desc);
    }
}

void AIMP3Manager::removeEntryFromPlayQueue(TrackDescription track_desc) // throws std::runtime_error
{
    using namespace AIMP3SDK;
    AIMP3SDK::HPLSENTRY entry_handle = getEntryHandle(track_desc, aimp3_playlist_manager_);
    HRESULT r = aimp3_playlist_manager_->QueueEntryRemove(entry_handle);
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << "Error " << r << " in "__FUNCTION__" with " << track_desc);
    }
}

const AIMP3Manager::PlaylistsListType& AIMP3Manager::getPlayLists() const
{
    return playlists_;
}

void AIMP3Manager::savePNGCoverToVector(TrackDescription track_desc, int cover_width, int cover_height, std::vector<BYTE>& image_data) const
{
    std::vector<BYTE> image_data_temp; // will be contain BMP image data.
    try {
        using namespace ImageUtils;
        std::auto_ptr<AIMPCoverImage> cover( getCoverImage(track_desc, cover_width, cover_height) );
        cover->saveToVector(AIMPCoverImage::PNG_IMAGE, image_data_temp);
    } catch (std::exception& e) {
        const std::string& str = MakeString() << "Error occured while cover saving to vector for " << track_desc << ". Reason: " << e.what();
        BOOST_LOG_SEV(logger(), error) << str;
        throw std::runtime_error(str);
    }

    // we got image, save it now.
    image_data.swap(image_data_temp);
}

void AIMP3Manager::savePNGCoverToFile(TrackDescription track_desc, int cover_width, int cover_height, const std::wstring& filename) const
{
    try {
        using namespace ImageUtils;
        std::auto_ptr<AIMPCoverImage> cover( getCoverImage(track_desc, cover_width, cover_height) );
        cover->saveToFile(AIMPCoverImage::PNG_IMAGE, filename);
    } catch (std::exception& e) {
        const std::string& str = MakeString() << "Error occured while cover saving to file for " << track_desc << ". Reason: " << e.what();
        BOOST_LOG_SEV(logger(), error) << str;
        throw std::runtime_error(str);
    }
}

std::auto_ptr<ImageUtils::AIMPCoverImage> AIMP3Manager::getCoverImage(TrackDescription track_desc, int cover_width, int cover_height) const
{
    if (cover_width < 0 || cover_height < 0) {
        throw std::invalid_argument(MakeString() << "Error in "__FUNCTION__ << ". Negative cover size.");
    }

    const PlaylistEntry& entry = getEntry(track_desc);
    //SIZE cover_size = { 0, 0 };
    //HBITMAP cover_bitmap_handle = aimp2_cover_art_manager_->GetCoverArtForFile(const_cast<PWCHAR>( entry.getFilename().c_str() ), &request_full_size);
    WCHAR coverart_filename_buffer[MAX_PATH + 1] = {0};
    HBITMAP cover_bitmap_handle = aimp3_coverart_manager_->CoverArtGetForFile(const_cast<PWCHAR>( entry.getFilename().c_str() ), NULL,
			                                                                  coverart_filename_buffer, MAX_PATH);
    if (coverart_filename_buffer[0] != 0) {
        coverart_filename_buffer[0] = coverart_filename_buffer[0];
    }

    // get real bitmap size
    const SIZE cover_full_size = ImageUtils::getBitmapSize(cover_bitmap_handle);
    if (cover_full_size.cx != 0 && cover_full_size.cy != 0) {
        SIZE cover_size;
        if (cover_width != 0 && cover_height != 0) {
            // specified size
            cover_size.cx = cover_width;
            cover_size.cy = cover_height;
        } else if (cover_width == 0 && cover_height == 0) {
            // original size
            cover_size.cx = cover_full_size.cx;
            cover_size.cy = cover_full_size.cy;
        } else if (cover_height == 0) {
            // specified width, proportional height
            cover_size.cx = cover_width;
            cover_size.cy = LONG( float(cover_full_size.cy) * float(cover_width) / float(cover_full_size.cx) );
        } else if (cover_width == 0) {
            // specified height, proportional width
            cover_size.cx = LONG( float(cover_full_size.cx) * float(cover_height) / float(cover_full_size.cy) );
            cover_size.cy = cover_height;
        }

        cover_bitmap_handle = aimp3_coverart_manager_->CoverArtGetForFile(const_cast<PWCHAR>( entry.getFilename().c_str() ), &cover_size,
			                                                              coverart_filename_buffer, MAX_PATH);
    }

    using namespace ImageUtils;
    return std::auto_ptr<AIMPCoverImage>( new AIMPCoverImage(cover_bitmap_handle) ); // do not close handle of AIMP bitmap.
}

const Playlist& AIMP3Manager::getPlaylist(PlaylistID playlist_id) const
{
    if (playlist_id == -1) { // treat ID -1 as active playlist.
        playlist_id = getActivePlaylist();
    }

    auto playlist_iterator( playlists_.find(playlist_id) );
    if ( playlist_iterator == playlists_.end() ) {
        throw std::runtime_error(MakeString() << "Error in "__FUNCTION__ << ": playlist with ID = " << playlist_id << " does not exist");
    }

    return playlist_iterator->second;
}

const PlaylistEntry& AIMP3Manager::getEntry(TrackDescription track_desc) const
{
    const Playlist& playlist = getPlaylist(track_desc.playlist_id);

    if (track_desc.track_id == -1) { // treat ID -1 as active track.
        if (   track_desc.playlist_id == -1
            || track_desc.playlist_id == getActivePlaylist()
            )
        {
            track_desc.track_id = getActiveEntry();
        }
    }
    const EntriesListType& entries = playlist.getEntries();
    if ( track_desc.track_id < 0 || static_cast<size_t>(track_desc.track_id) >= entries.size() ) {
        throw std::runtime_error(MakeString() << "Error in "__FUNCTION__ << ". Entry " << track_desc << " does not exist");
    }

    return entries[track_desc.track_id]; // currently track ID is simple index in entries list.
}

void AIMP3Manager::notifyAllExternalListeners(EVENTS event) const
{
    BOOST_FOREACH(const auto& listener_pair, external_listeners_) {
        const EventsListener& listener = listener_pair.second;
        listener(event);
    }
}

AIMP3Manager::EventsListenerID AIMP3Manager::registerListener(AIMP3Manager::EventsListener listener)
{
    external_listeners_[next_listener_id_] = listener;
    assert(next_listener_id_ != UINT32_MAX);
    return ++next_listener_id_; // get next unique listener ID using simple increment.
}

void AIMP3Manager::unRegisterListener(AIMP3Manager::EventsListenerID listener_id)
{
    external_listeners_.erase(listener_id);
}

std::wstring AIMP3Manager::getFormattedEntryTitle(const PlaylistEntry& entry, const std::string& format_string_utf8) const // throw std::invalid_argument
{
    std::wstring wformat_string( StringEncoding::utf8_to_utf16(format_string_utf8) );
    
    PWCHAR formatted_string = nullptr;
    HRESULT r = aimp3_playlist_manager_->FormatString( const_cast<PWCHAR>( wformat_string.c_str() ),
                                                       wformat_string.length(),
                                                       0, // AIMP_PLAYLIST_FORMAT_MODE_PREVIEW
                                                       nullptr,
                                                       &formatted_string
                                                      );
    if (S_OK != r) {
        throw std::runtime_error(MakeString() << "Error " << r << " in "__FUNCTION__" with " << format_string_utf8);
    }

    return formatted_string;
}

} // namespace AIMPPlayer