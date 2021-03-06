#include "soundmanagerimp.hpp"

#include <iostream>
#include <algorithm>
#include <map>

#include <components/misc/rng.hpp>

#include <components/vfs/manager.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwbase/statemanager.hpp"

#include "../mwworld/esmstore.hpp"
#include "../mwworld/cellstore.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "sound_output.hpp"
#include "sound_buffer.hpp"
#include "sound_decoder.hpp"
#include "sound.hpp"

#include "openal_output.hpp"
#define SOUND_OUT "OpenAL"
#include "ffmpeg_decoder.hpp"
#ifndef SOUND_IN
#define SOUND_IN "FFmpeg"
#endif


namespace MWSound
{
    SoundManager::SoundManager(const VFS::Manager* vfs, bool useSound)
        : mVFS(vfs)
        , mOutput(new DEFAULT_OUTPUT(*this))
        , mMasterVolume(1.0f)
        , mSFXVolume(1.0f)
        , mMusicVolume(1.0f)
        , mVoiceVolume(1.0f)
        , mFootstepsVolume(1.0f)
        , mSoundBuffers(new SoundBufferList::element_type())
        , mBufferCacheSize(0)
        , mListenerUnderwater(false)
        , mListenerPos(0,0,0)
        , mListenerDir(1,0,0)
        , mListenerUp(0,0,1)
        , mPausedSoundTypes(0)
    {
        if(!useSound)
            return;

        mMasterVolume = Settings::Manager::getFloat("master volume", "Sound");
        mMasterVolume = std::min(std::max(mMasterVolume, 0.0f), 1.0f);
        mSFXVolume = Settings::Manager::getFloat("sfx volume", "Sound");
        mSFXVolume = std::min(std::max(mSFXVolume, 0.0f), 1.0f);
        mMusicVolume = Settings::Manager::getFloat("music volume", "Sound");
        mMusicVolume = std::min(std::max(mMusicVolume, 0.0f), 1.0f);
        mVoiceVolume = Settings::Manager::getFloat("voice volume", "Sound");
        mVoiceVolume = std::min(std::max(mVoiceVolume, 0.0f), 1.0f);
        mFootstepsVolume = Settings::Manager::getFloat("footsteps volume", "Sound");
        mFootstepsVolume = std::min(std::max(mFootstepsVolume, 0.0f), 1.0f);

        mBufferCacheMin = std::max(Settings::Manager::getInt("buffer cache min", "Sound"), 1);
        mBufferCacheMax = std::max(Settings::Manager::getInt("buffer cache max", "Sound"), 1);
        mBufferCacheMax *= 1024*1024;
        mBufferCacheMin = std::min(mBufferCacheMin*1024*1024, mBufferCacheMax);

        std::cout << "Sound output: " << SOUND_OUT << std::endl;
        std::cout << "Sound decoder: " << SOUND_IN << std::endl;

        try
        {
            std::vector<std::string> names = mOutput->enumerate();
            std::cout <<"Enumerated output devices:"<< std::endl;
            for(size_t i = 0;i < names.size();i++)
                std::cout <<"  "<<names[i]<< std::endl;

            std::string devname = Settings::Manager::getString("device", "Sound");
            try
            {
                mOutput->init(devname);
            }
            catch(std::exception &e)
            {
                if(devname.empty())
                    throw;
                std::cerr <<"Failed to open device \""<<devname<<"\": " << e.what() << std::endl;
                mOutput->init();
                Settings::Manager::setString("device", "Sound", "");
            }
        }
        catch(std::exception &e)
        {
            std::cout <<"Sound init failed: "<<e.what()<< std::endl;
        }
    }

    SoundManager::~SoundManager()
    {
        clear();
        if(mOutput->isInitialized())
        {
            SoundBufferList::element_type::iterator sfxiter = mSoundBuffers->begin();
            for(;sfxiter != mSoundBuffers->end();++sfxiter)
            {
                if(sfxiter->mHandle)
                    mOutput->unloadSound(sfxiter->mHandle);
                sfxiter->mHandle = 0;
            }
            mUnusedBuffers.clear();
        }
        mOutput.reset();
    }

    // Return a new decoder instance, used as needed by the output implementations
    DecoderPtr SoundManager::getDecoder()
    {
        return DecoderPtr(new DEFAULT_DECODER (mVFS));
    }

    Sound_Buffer *SoundManager::insertSound(const std::string &soundId, const ESM::Sound *sound)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        static const float fAudioDefaultMinDistance = world->getStore().get<ESM::GameSetting>().find("fAudioDefaultMinDistance")->getFloat();
        static const float fAudioDefaultMaxDistance = world->getStore().get<ESM::GameSetting>().find("fAudioDefaultMaxDistance")->getFloat();
        static const float fAudioMinDistanceMult = world->getStore().get<ESM::GameSetting>().find("fAudioMinDistanceMult")->getFloat();
        static const float fAudioMaxDistanceMult = world->getStore().get<ESM::GameSetting>().find("fAudioMaxDistanceMult")->getFloat();
        float volume, min, max;

        volume = static_cast<float>(pow(10.0, (sound->mData.mVolume / 255.0*3348.0 - 3348.0) / 2000.0));
        if(sound->mData.mMinRange == 0 && sound->mData.mMaxRange == 0)
        {
            min = fAudioDefaultMinDistance;
            max = fAudioDefaultMaxDistance;
        }
        else
        {
            min = sound->mData.mMinRange;
            max = sound->mData.mMaxRange;
        }

        min *= fAudioMinDistanceMult;
        max *= fAudioMaxDistanceMult;
        min = std::max(min, 1.0f);
        max = std::max(min, max);

        Sound_Buffer *sfx = &*mSoundBuffers->insert(mSoundBuffers->end(),
            Sound_Buffer("Sound/"+sound->mSound, volume, min, max)
        );
        mVFS->normalizeFilename(sfx->mResourceName);

        mBufferNameMap.insert(std::make_pair(soundId, sfx));

        return sfx;
    }

    // Lookup a soundId for its sound data (resource name, local volume,
    // minRange, and maxRange)
    Sound_Buffer *SoundManager::lookupSound(const std::string &soundId) const
    {
        NameBufferMap::const_iterator snd = mBufferNameMap.find(soundId);
        if(snd != mBufferNameMap.end()) return snd->second;
        return 0;
    }

    // Lookup a soundId for its sound data (resource name, local volume,
    // minRange, and maxRange), and ensure it's ready for use.
    Sound_Buffer *SoundManager::loadSound(const std::string &soundId)
    {
        Sound_Buffer *sfx;
        NameBufferMap::const_iterator snd = mBufferNameMap.find(soundId);
        if(snd != mBufferNameMap.end())
            sfx = snd->second;
        else
        {
            MWBase::World *world = MWBase::Environment::get().getWorld();
            const ESM::Sound *sound = world->getStore().get<ESM::Sound>().find(soundId);
            sfx = insertSound(soundId, sound);
        }

        if(!sfx->mHandle)
        {
            sfx->mHandle = mOutput->loadSound(sfx->mResourceName);
            mBufferCacheSize += mOutput->getSoundDataSize(sfx->mHandle);

            if(mBufferCacheSize > mBufferCacheMax)
            {
                do {
                    if(mUnusedBuffers.empty())
                    {
                        std::cerr<< "No unused sound buffers to free, using "<<mBufferCacheSize<<" bytes!" <<std::endl;
                        break;
                    }
                    Sound_Buffer *unused = mUnusedBuffers.back();

                    mBufferCacheSize -= mOutput->getSoundDataSize(unused->mHandle);
                    mOutput->unloadSound(unused->mHandle);
                    unused->mHandle = 0;

                    mUnusedBuffers.pop_back();
                } while(mBufferCacheSize > mBufferCacheMin);
            }
            mUnusedBuffers.push_front(sfx);
        }

        return sfx;
    }

    DecoderPtr SoundManager::loadVoice(const std::string &voicefile, Sound_Loudness **lipdata)
    {
        DecoderPtr decoder = getDecoder();
        // Workaround: Bethesda at some point converted some of the files to mp3, but the references were kept as .wav.
        if(decoder->mResourceMgr->exists(voicefile))
            decoder->open(voicefile);
        else
        {
            std::string file = voicefile;
            std::string::size_type pos = file.rfind('.');
            if(pos != std::string::npos)
                file = file.substr(0, pos)+".mp3";
            decoder->open(file);
        }

        NameLoudnessRefMap::iterator lipiter = mVoiceLipNameMap.find(voicefile);
        if(lipiter != mVoiceLipNameMap.end())
        {
            *lipdata = lipiter->second;
            return decoder;
        }

        mVoiceLipBuffers.insert(mVoiceLipBuffers.end(), Sound_Loudness());
        lipiter = mVoiceLipNameMap.insert(
            std::make_pair(voicefile, &mVoiceLipBuffers.back())
        ).first;

        mOutput->loadLoudnessAsync(decoder, lipiter->second);

        *lipdata = lipiter->second;
        return decoder;
    }

    MWBase::SoundStreamPtr SoundManager::playVoice(DecoderPtr decoder, const osg::Vec3f &pos, bool playlocal)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        static const float fAudioMinDistanceMult = world->getStore().get<ESM::GameSetting>().find("fAudioMinDistanceMult")->getFloat();
        static const float fAudioMaxDistanceMult = world->getStore().get<ESM::GameSetting>().find("fAudioMaxDistanceMult")->getFloat();
        static const float fAudioVoiceDefaultMinDistance = world->getStore().get<ESM::GameSetting>().find("fAudioVoiceDefaultMinDistance")->getFloat();
        static const float fAudioVoiceDefaultMaxDistance = world->getStore().get<ESM::GameSetting>().find("fAudioVoiceDefaultMaxDistance")->getFloat();
        static float minDistance = std::max(fAudioVoiceDefaultMinDistance * fAudioMinDistanceMult, 1.0f);
        static float maxDistance = std::max(fAudioVoiceDefaultMaxDistance * fAudioMaxDistanceMult, minDistance);

        MWBase::SoundStreamPtr sound;
        float basevol = volumeFromType(Play_TypeVoice);
        if(playlocal)
        {
            sound.reset(new Stream(1.0f, basevol, 1.0f, Play_Normal|Play_TypeVoice|Play_2D));
            mOutput->streamSound(decoder, sound);
        }
        else
        {
            sound.reset(new Stream(pos, 1.0f, basevol, 1.0f, minDistance, maxDistance,
                                   Play_Normal|Play_TypeVoice|Play_3D));
            mOutput->streamSound3D(decoder, sound);
        }
        return sound;
    }


    // Gets the combined volume settings for the given sound type
    float SoundManager::volumeFromType(PlayType type) const
    {
        float volume = mMasterVolume;
        switch(type)
        {
            case Play_TypeSfx:
                volume *= mSFXVolume;
                break;
            case Play_TypeVoice:
                volume *= mVoiceVolume;
                break;
            case Play_TypeFoot:
                volume *= mFootstepsVolume;
                break;
            case Play_TypeMusic:
                volume *= mMusicVolume;
                break;
            case Play_TypeMask:
                break;
            default:
                break;
        }
        return volume;
    }


    void SoundManager::stopMusic()
    {
        if(mMusic)
            mOutput->stopStream(mMusic);
        mMusic.reset();
    }

    void SoundManager::streamMusicFull(const std::string& filename)
    {
        if(!mOutput->isInitialized())
            return;
        std::cout <<"Playing "<<filename<< std::endl;
        mLastPlayedMusic = filename;
        try {
            stopMusic();

            DecoderPtr decoder = getDecoder();
            decoder->open(filename);

            mMusic.reset(new Stream(1.0f, volumeFromType(Play_TypeMusic), 1.0f,
                                    Play_NoEnv|Play_TypeMusic|Play_2D));
            mOutput->streamSound(decoder, mMusic);
        }
        catch(std::exception &e) {
            std::cout << "Music Error: " << e.what() << "\n";
            mMusic.reset();
        }
    }

    void SoundManager::streamMusic(const std::string& filename)
    {
        streamMusicFull("Music/"+filename);
    }

    void SoundManager::startRandomTitle()
    {
        std::vector<std::string> filelist;
        if (mMusicFiles.find(mCurrentPlaylist) == mMusicFiles.end())
        {
            const std::map<std::string, VFS::File*>& index = mVFS->getIndex();

            std::string pattern = "Music/" + mCurrentPlaylist;
            mVFS->normalizeFilename(pattern);

            std::map<std::string, VFS::File*>::const_iterator found = index.lower_bound(pattern);
            while (found != index.end())
            {
                if (found->first.size() >= pattern.size() && found->first.substr(0, pattern.size()) == pattern)
                    filelist.push_back(found->first);
                else
                    break;
                ++found;
            }

            mMusicFiles[mCurrentPlaylist] = filelist;

        }
        else
            filelist = mMusicFiles[mCurrentPlaylist];

        if(!filelist.size())
            return;

        int i = Misc::Rng::rollDice(filelist.size());

        // Don't play the same music track twice in a row
        if (filelist[i] == mLastPlayedMusic)
        {
            i = (i+1) % filelist.size();
        }

        streamMusicFull(filelist[i]);
    }

    bool SoundManager::isMusicPlaying()
    {
        return mMusic && mOutput->isStreamPlaying(mMusic);
    }

    void SoundManager::playPlaylist(const std::string &playlist)
    {
        mCurrentPlaylist = playlist;
        startRandomTitle();
    }


    void SoundManager::say(const MWWorld::Ptr &ptr, const std::string &filename)
    {
        if(!mOutput->isInitialized())
            return;
        try
        {
            std::string voicefile = "Sound/"+filename;
            const ESM::Position &pos = ptr.getRefData().getPosition();
            const osg::Vec3f objpos(pos.asVec3());

            Sound_Loudness *loudness;
            mVFS->normalizeFilename(voicefile);
            DecoderPtr decoder = loadVoice(voicefile, &loudness);

            if(!loudness->isReady())
                mPendingSaySounds[ptr] = std::make_pair(decoder, loudness);
            else
            {
                MWBase::SoundStreamPtr sound = playVoice(decoder, objpos, (ptr == MWMechanics::getPlayer()));
                mActiveSaySounds[ptr] = std::make_pair(sound, loudness);
            }
        }
        catch(std::exception &e)
        {
            std::cout <<"Sound Error: "<<e.what()<< std::endl;
        }
    }

    float SoundManager::getSaySoundLoudness(const MWWorld::Ptr &ptr) const
    {
        SaySoundMap::const_iterator snditer = mActiveSaySounds.find(ptr);
        if(snditer != mActiveSaySounds.end())
        {
            MWBase::SoundStreamPtr sound = snditer->second.first;
            Sound_Loudness *loudness = snditer->second.second;
            float sec = mOutput->getStreamOffset(sound);
            return loudness->getLoudnessAtTime(sec);
        }

        return 0.0f;
    }

    void SoundManager::say(const std::string& filename)
    {
        if(!mOutput->isInitialized())
            return;
        try
        {
            std::string voicefile = "Sound/"+filename;

            Sound_Loudness *loudness;
            mVFS->normalizeFilename(voicefile);
            DecoderPtr decoder = loadVoice(voicefile, &loudness);

            if(!loudness->isReady())
                mPendingSaySounds[MWWorld::Ptr()] = std::make_pair(decoder, loudness);
            else
            {
                MWBase::SoundStreamPtr sound = playVoice(decoder, osg::Vec3f(), true);
                mActiveSaySounds[MWWorld::Ptr()] = std::make_pair(sound, loudness);
            }
        }
        catch(std::exception &e)
        {
            std::cout <<"Sound Error: "<<e.what()<< std::endl;
        }
    }

    bool SoundManager::sayDone(const MWWorld::Ptr &ptr) const
    {
        SaySoundMap::const_iterator snditer = mActiveSaySounds.find(ptr);
        if(snditer != mActiveSaySounds.end())
        {
            if(mOutput->isStreamPlaying(snditer->second.first))
                return false;
            return true;
        }
        return mPendingSaySounds.find(ptr) == mPendingSaySounds.end();
    }

    void SoundManager::stopSay(const MWWorld::Ptr &ptr)
    {
        SaySoundMap::iterator snditer = mActiveSaySounds.find(ptr);
        if(snditer != mActiveSaySounds.end())
        {
            mOutput->stopStream(snditer->second.first);
            mActiveSaySounds.erase(snditer);
        }
        mPendingSaySounds.erase(ptr);
    }


    MWBase::SoundStreamPtr SoundManager::playTrack(const DecoderPtr& decoder, PlayType type)
    {
        MWBase::SoundStreamPtr track;
        if(!mOutput->isInitialized())
            return track;
        try
        {
            track.reset(new Stream(1.0f, volumeFromType(type), 1.0f, Play_NoEnv|type|Play_2D));
            mOutput->streamSound(decoder, track);

            TrackList::iterator iter = std::lower_bound(mActiveTracks.begin(), mActiveTracks.end(), track);
            mActiveTracks.insert(iter, track);
        }
        catch(std::exception &e)
        {
            std::cout <<"Sound Error: "<<e.what()<< std::endl;
        }
        return track;
    }

    void SoundManager::stopTrack(MWBase::SoundStreamPtr stream)
    {
        mOutput->stopStream(stream);
        TrackList::iterator iter = std::lower_bound(mActiveTracks.begin(), mActiveTracks.end(), stream);
        if(iter != mActiveTracks.end() && *iter == stream)
            mActiveTracks.erase(iter);
    }

    double SoundManager::getTrackTimeDelay(MWBase::SoundStreamPtr stream)
    {
        return mOutput->getStreamDelay(stream);
    }


    MWBase::SoundPtr SoundManager::playSound(const std::string& soundId, float volume, float pitch, PlayType type, PlayMode mode, float offset)
    {
        MWBase::SoundPtr sound;
        if(!mOutput->isInitialized())
            return sound;
        try
        {
            Sound_Buffer *sfx = loadSound(Misc::StringUtils::lowerCase(soundId));
            float basevol = volumeFromType(type);

            sound.reset(new Sound(volume * sfx->mVolume, basevol, pitch, mode|type|Play_2D));
            mOutput->playSound(sound, sfx->mHandle, offset);
            if(sfx->mUses++ == 0)
            {
                SoundList::iterator iter = std::find(mUnusedBuffers.begin(), mUnusedBuffers.end(), sfx);
                if(iter != mUnusedBuffers.end())
                    mUnusedBuffers.erase(iter);
            }
            mActiveSounds[MWWorld::Ptr()].push_back(std::make_pair(sound, sfx));
        }
        catch(std::exception&)
        {
            //std::cout <<"Sound Error: "<<e.what()<< std::endl;
            sound.reset();
        }
        return sound;
    }

    MWBase::SoundPtr SoundManager::playSound3D(const MWWorld::Ptr &ptr, const std::string& soundId,
                                               float volume, float pitch, PlayType type, PlayMode mode, float offset)
    {
        MWBase::SoundPtr sound;
        if(!mOutput->isInitialized())
            return sound;
        try
        {
            // Look up the sound in the ESM data
            Sound_Buffer *sfx = loadSound(Misc::StringUtils::lowerCase(soundId));
            float basevol = volumeFromType(type);
            const ESM::Position &pos = ptr.getRefData().getPosition();
            const osg::Vec3f objpos(pos.asVec3());

            if((mode&Play_RemoveAtDistance) && (mListenerPos-objpos).length2() > 2000*2000)
                return MWBase::SoundPtr();

            if(!(mode&Play_NoPlayerLocal) && ptr == MWMechanics::getPlayer())
            {
                sound.reset(new Sound(volume * sfx->mVolume, basevol, pitch, mode|type|Play_2D));
                mOutput->playSound(sound, sfx->mHandle, offset);
            }
            else
            {
                sound.reset(new Sound(objpos, volume * sfx->mVolume, basevol, pitch,
                                      sfx->mMinDist, sfx->mMaxDist, mode|type|Play_3D));
                mOutput->playSound3D(sound, sfx->mHandle, offset);
            }
            if(sfx->mUses++ == 0)
            {
                SoundList::iterator iter = std::find(mUnusedBuffers.begin(), mUnusedBuffers.end(), sfx);
                if(iter != mUnusedBuffers.end())
                    mUnusedBuffers.erase(iter);
            }
            mActiveSounds[ptr].push_back(std::make_pair(sound, sfx));
        }
        catch(std::exception&)
        {
            //std::cout <<"Sound Error: "<<e.what()<< std::endl;
            sound.reset();
        }
        return sound;
    }

    MWBase::SoundPtr SoundManager::playSound3D(const osg::Vec3f& initialPos, const std::string& soundId,
                                               float volume, float pitch, PlayType type, PlayMode mode, float offset)
    {
        MWBase::SoundPtr sound;
        if(!mOutput->isInitialized())
            return sound;
        try
        {
            // Look up the sound in the ESM data
            Sound_Buffer *sfx = loadSound(Misc::StringUtils::lowerCase(soundId));
            float basevol = volumeFromType(type);

            sound.reset(new Sound(initialPos, volume * sfx->mVolume, basevol, pitch,
                                  sfx->mMinDist, sfx->mMaxDist, mode|type|Play_3D));
            mOutput->playSound3D(sound, sfx->mHandle, offset);
            if(sfx->mUses++ == 0)
            {
                SoundList::iterator iter = std::find(mUnusedBuffers.begin(), mUnusedBuffers.end(), sfx);
                if(iter != mUnusedBuffers.end())
                    mUnusedBuffers.erase(iter);
            }
            mActiveSounds[MWWorld::Ptr()].push_back(std::make_pair(sound, sfx));
        }
        catch(std::exception &)
        {
            //std::cout <<"Sound Error: "<<e.what()<< std::endl;
            sound.reset();
        }
        return sound;
    }

    void SoundManager::stopSound(MWBase::SoundPtr sound)
    {
        if (sound.get())
            mOutput->stopSound(sound);
    }

    void SoundManager::stopSound3D(const MWWorld::Ptr &ptr, const std::string& soundId)
    {
        SoundMap::iterator snditer = mActiveSounds.find(ptr);
        if(snditer != mActiveSounds.end())
        {
            Sound_Buffer *sfx = loadSound(Misc::StringUtils::lowerCase(soundId));
            SoundBufferRefPairList::iterator sndidx = snditer->second.begin();
            for(;sndidx != snditer->second.end();++sndidx)
            {
                if(sndidx->second == sfx)
                    mOutput->stopSound(sndidx->first);
            }
        }
    }

    void SoundManager::stopSound3D(const MWWorld::Ptr &ptr)
    {
        SoundMap::iterator snditer = mActiveSounds.find(ptr);
        if(snditer != mActiveSounds.end())
        {
            SoundBufferRefPairList::iterator sndidx = snditer->second.begin();
            for(;sndidx != snditer->second.end();++sndidx)
                mOutput->stopSound(sndidx->first);
        }
    }

    void SoundManager::stopSound(const MWWorld::CellStore *cell)
    {
        SoundMap::iterator snditer = mActiveSounds.begin();
        while(snditer != mActiveSounds.end())
        {
            if(snditer->first != MWWorld::Ptr() &&
               snditer->first != MWMechanics::getPlayer() &&
               snditer->first.getCell() == cell)
            {
                SoundBufferRefPairList::iterator sndidx = snditer->second.begin();
                for(;sndidx != snditer->second.end();++sndidx)
                    mOutput->stopSound(sndidx->first);
            }
            ++snditer;
        }
        SaySoundMap::iterator sayiter = mActiveSaySounds.begin();
        while(sayiter != mActiveSaySounds.end())
        {
            if(sayiter->first != MWWorld::Ptr() &&
               sayiter->first != MWMechanics::getPlayer() &&
               sayiter->first.getCell() == cell)
            {
                mOutput->stopStream(sayiter->second.first);
            }
            ++sayiter;
        }
    }

    void SoundManager::stopSound(const std::string& soundId)
    {
        SoundMap::iterator snditer = mActiveSounds.find(MWWorld::Ptr());
        if(snditer != mActiveSounds.end())
        {
            Sound_Buffer *sfx = loadSound(Misc::StringUtils::lowerCase(soundId));
            SoundBufferRefPairList::iterator sndidx = snditer->second.begin();
            for(;sndidx != snditer->second.end();++sndidx)
            {
                if(sndidx->second == sfx)
                    mOutput->stopSound(sndidx->first);
            }
        }
    }

    void SoundManager::fadeOutSound3D(const MWWorld::Ptr &ptr,
            const std::string& soundId, float duration)
    {
        SoundMap::iterator snditer = mActiveSounds.find(ptr);
        if(snditer != mActiveSounds.end())
        {
            Sound_Buffer *sfx = loadSound(Misc::StringUtils::lowerCase(soundId));
            SoundBufferRefPairList::iterator sndidx = snditer->second.begin();
            for(;sndidx != snditer->second.end();++sndidx)
            {
                if(sndidx->second == sfx)
                    sndidx->first->setFadeout(duration);
            }
        }
    }

    bool SoundManager::getSoundPlaying(const MWWorld::Ptr &ptr, const std::string& soundId) const
    {
        SoundMap::const_iterator snditer = mActiveSounds.find(ptr);
        if(snditer != mActiveSounds.end())
        {
            Sound_Buffer *sfx = lookupSound(Misc::StringUtils::lowerCase(soundId));
            SoundBufferRefPairList::const_iterator sndidx = snditer->second.begin();
            for(;sndidx != snditer->second.end();++sndidx)
            {
                if(sndidx->second == sfx && mOutput->isSoundPlaying(sndidx->first))
                    return true;
            }
        }
        return false;
    }


    void SoundManager::pauseSounds(int types)
    {
        if(mOutput->isInitialized())
        {
            types &= Play_TypeMask;
            mOutput->pauseSounds(types);
            mPausedSoundTypes |= types;
        }
    }

    void SoundManager::resumeSounds(int types)
    {
        if(mOutput->isInitialized())
        {
            types &= types&Play_TypeMask&mPausedSoundTypes;
            mOutput->resumeSounds(types);
            mPausedSoundTypes &= ~types;
        }
    }


    void SoundManager::updateRegionSound(float duration)
    {
        static float sTimeToNextEnvSound = 0.0f;
        static int total = 0;
        static std::string regionName = "";
        static float sTimePassed = 0.0;
        MWBase::World *world = MWBase::Environment::get().getWorld();
        const MWWorld::Ptr player = world->getPlayerPtr();
        const ESM::Cell *cell = player.getCell()->getCell();

        sTimePassed += duration;
        if(!cell->isExterior() || sTimePassed < sTimeToNextEnvSound)
            return;

        float a = Misc::Rng::rollClosedProbability();
        // NOTE: We should use the "Minimum Time Between Environmental Sounds" and
        // "Maximum Time Between Environmental Sounds" fallback settings here.
        sTimeToNextEnvSound = 5.0f*a + 15.0f*(1.0f-a);
        sTimePassed = 0;

        if(regionName != cell->mRegion)
        {
            regionName = cell->mRegion;
            total = 0;
        }

        const ESM::Region *regn = world->getStore().get<ESM::Region>().search(regionName);
        if(regn == NULL)
            return;

        std::vector<ESM::Region::SoundRef>::const_iterator soundIter;
        if(total == 0)
        {
            soundIter = regn->mSoundList.begin();
            while(soundIter != regn->mSoundList.end())
            {
                total += (int)soundIter->mChance;
                ++soundIter;
            }
            if(total == 0)
                return;
        }

        int r = Misc::Rng::rollDice(total);
        int pos = 0;

        soundIter = regn->mSoundList.begin();
        while(soundIter != regn->mSoundList.end())
        {
            if(r - pos < soundIter->mChance)
            {
                playSound(soundIter->mSound.toString(), 1.0f, 1.0f);
                break;
            }
            pos += soundIter->mChance;

            ++soundIter;
        }
    }

    void SoundManager::updateSounds(float duration)
    {
        static float timePassed = 0.0;

        timePassed += duration;
        if(timePassed < (1.0f/30.0f))
            return;
        duration = timePassed;
        timePassed = 0.0f;

        // Make sure music is still playing
        if(!isMusicPlaying())
            startRandomTitle();

        Environment env = Env_Normal;
        if (mListenerUnderwater)
            env = Env_Underwater;
        else if(mUnderwaterSound)
        {
            mOutput->stopSound(mUnderwaterSound);
            mUnderwaterSound.reset();
        }

        mOutput->startUpdate();
        mOutput->updateListener(
            mListenerPos,
            mListenerDir,
            mListenerUp,
            env
        );

        if(mListenerUnderwater)
        {
            // Play underwater sound (after updating listener)
            if(!(mUnderwaterSound && mOutput->isSoundPlaying(mUnderwaterSound)))
                mUnderwaterSound = playSound("Underwater", 1.0f, 1.0f, Play_TypeSfx, Play_LoopNoEnv);
        }

        // Check if any sounds are finished playing, and trash them
        SoundMap::iterator snditer = mActiveSounds.begin();
        while(snditer != mActiveSounds.end())
        {
            SoundBufferRefPairList::iterator sndidx = snditer->second.begin();
            while(sndidx != snditer->second.end())
            {
                MWWorld::Ptr ptr = snditer->first;
                MWBase::SoundPtr sound = sndidx->first;
                if(!ptr.isEmpty() && sound->getIs3D())
                {
                    const ESM::Position &pos = ptr.getRefData().getPosition();
                    const osg::Vec3f objpos(pos.asVec3());
                    sound->setPosition(objpos);

                    if(sound->getDistanceCull())
                    {
                        if((mListenerPos - objpos).length2() > 2000*2000)
                            mOutput->stopSound(sound);
                    }
                }

                if(!mOutput->isSoundPlaying(sound))
                {
                    mOutput->stopSound(sound);
                    Sound_Buffer *sfx = sndidx->second;
                    if(sfx->mUses-- == 1)
                        mUnusedBuffers.push_front(sfx);
                    sndidx = snditer->second.erase(sndidx);
                }
                else
                {
                    sound->updateFade(duration);

                    mOutput->updateSound(sound);
                    ++sndidx;
                }
            }
            if(snditer->second.empty())
                mActiveSounds.erase(snditer++);
            else
                ++snditer;
        }

        SayDecoderMap::iterator penditer = mPendingSaySounds.begin();
        while(penditer != mPendingSaySounds.end())
        {
            Sound_Loudness *loudness = penditer->second.second;
            if(loudness->isReady())
            {
                try {
                    DecoderPtr decoder = penditer->second.first;
                    decoder->rewind();

                    MWBase::SoundStreamPtr sound;
                    MWWorld::Ptr ptr = penditer->first;
                    if(ptr == MWWorld::Ptr())
                        sound = playVoice(decoder, osg::Vec3f(), true);
                    else
                    {
                        const ESM::Position &pos = ptr.getRefData().getPosition();
                        const osg::Vec3f objpos(pos.asVec3());

                        sound = playVoice(decoder, objpos, (ptr == MWMechanics::getPlayer()));
                    }
                    mActiveSaySounds[ptr] = std::make_pair(sound, loudness);
                }
                catch(std::exception &e) {
                    std::cerr<< "Sound Error: "<<e.what() <<std::endl;
                }

                mPendingSaySounds.erase(penditer++);
            }
            else
                ++penditer;
        }

        SaySoundMap::iterator sayiter = mActiveSaySounds.begin();
        while(sayiter != mActiveSaySounds.end())
        {
            MWWorld::Ptr ptr = sayiter->first;
            MWBase::SoundStreamPtr sound = sayiter->second.first;
            if(!ptr.isEmpty() && sound->getIs3D())
            {
                const ESM::Position &pos = ptr.getRefData().getPosition();
                const osg::Vec3f objpos(pos.asVec3());
                sound->setPosition(objpos);

                if(sound->getDistanceCull())
                {
                    if((mListenerPos - objpos).length2() > 2000*2000)
                        mOutput->stopStream(sound);
                }
            }

            if(!mOutput->isStreamPlaying(sound))
            {
                mOutput->stopStream(sound);
                mActiveSaySounds.erase(sayiter++);
            }
            else
            {
                sound->updateFade(duration);

                mOutput->updateStream(sound);
                ++sayiter;
            }
        }

        TrackList::iterator trkiter = mActiveTracks.begin();
        for(;trkiter != mActiveTracks.end();++trkiter)
        {
            MWBase::SoundStreamPtr sound = *trkiter;
            if(!mOutput->isStreamPlaying(sound))
            {
                mOutput->stopStream(sound);
                trkiter = mActiveTracks.erase(trkiter);
            }
            else
            {
                sound->updateFade(duration);

                mOutput->updateStream(sound);
                ++trkiter;
            }
        }
        mOutput->finishUpdate();
    }


    void SoundManager::update(float duration)
    {
        if(!mOutput->isInitialized())
            return;

        if (MWBase::Environment::get().getStateManager()->getState()!=
            MWBase::StateManager::State_NoGame)
        {
            updateSounds(duration);
            updateRegionSound(duration);
        }
    }


    void SoundManager::processChangedSettings(const Settings::CategorySettingVector& settings)
    {
        mMasterVolume = Settings::Manager::getFloat("master volume", "Sound");
        mMusicVolume = Settings::Manager::getFloat("music volume", "Sound");
        mSFXVolume = Settings::Manager::getFloat("sfx volume", "Sound");
        mFootstepsVolume = Settings::Manager::getFloat("footsteps volume", "Sound");
        mVoiceVolume = Settings::Manager::getFloat("voice volume", "Sound");

        if(!mOutput->isInitialized())
            return;
        mOutput->startUpdate();
        SoundMap::iterator snditer = mActiveSounds.begin();
        for(;snditer != mActiveSounds.end();++snditer)
        {
            SoundBufferRefPairList::iterator sndidx = snditer->second.begin();
            for(;sndidx != snditer->second.end();++sndidx)
            {
                MWBase::SoundPtr sound = sndidx->first;
                sound->setBaseVolume(volumeFromType(sound->getPlayType()));
                mOutput->updateSound(sound);
            }
        }
        SaySoundMap::iterator sayiter = mActiveSaySounds.begin();
        for(;sayiter != mActiveSaySounds.end();++sayiter)
        {
            MWBase::SoundStreamPtr sound = sayiter->second.first;
            sound->setBaseVolume(volumeFromType(sound->getPlayType()));
            mOutput->updateStream(sound);
        }
        TrackList::iterator trkiter = mActiveTracks.begin();
        for(;trkiter != mActiveTracks.end();++trkiter)
        {
            MWBase::SoundStreamPtr sound = *trkiter;
            sound->setBaseVolume(volumeFromType(sound->getPlayType()));
            mOutput->updateStream(sound);
        }
        if(mMusic)
        {
            mMusic->setBaseVolume(volumeFromType(mMusic->getPlayType()));
            mOutput->updateStream(mMusic);
        }
        mOutput->finishUpdate();
    }

    void SoundManager::setListenerPosDir(const osg::Vec3f &pos, const osg::Vec3f &dir, const osg::Vec3f &up, bool underwater)
    {
        mListenerPos = pos;
        mListenerDir = dir;
        mListenerUp  = up;

        mListenerUnderwater = underwater;
    }

    void SoundManager::updatePtr(const MWWorld::Ptr &old, const MWWorld::Ptr &updated)
    {
        SoundMap::iterator snditer = mActiveSounds.find(old);
        if(snditer != mActiveSounds.end())
        {
            SoundBufferRefPairList sndlist = snditer->second;
            mActiveSounds.erase(snditer);
            mActiveSounds[updated] = sndlist;
        }
        SaySoundMap::iterator sayiter = mActiveSaySounds.find(old);
        if(sayiter != mActiveSaySounds.end())
        {
            SoundLoudnessPair sndlist = sayiter->second;
            mActiveSaySounds.erase(sayiter);
            mActiveSaySounds[updated] = sndlist;
        }
        SayDecoderMap::iterator penditer = mPendingSaySounds.find(old);
        if(penditer != mPendingSaySounds.end())
        {
            DecoderLoudnessPair dl = penditer->second;
            mPendingSaySounds.erase(penditer);
            mPendingSaySounds[updated] = dl;
        }
    }

    // Default readAll implementation, for decoders that can't do anything
    // better
    void Sound_Decoder::readAll(std::vector<char> &output)
    {
        size_t total = output.size();
        size_t got;

        output.resize(total+32768);
        while((got=read(&output[total], output.size()-total)) > 0)
        {
            total += got;
            output.resize(total*2);
        }
        output.resize(total);
    }


    const char *getSampleTypeName(SampleType type)
    {
        switch(type)
        {
            case SampleType_UInt8: return "U8";
            case SampleType_Int16: return "S16";
            case SampleType_Float32: return "Float32";
        }
        return "(unknown sample type)";
    }

    const char *getChannelConfigName(ChannelConfig config)
    {
        switch(config)
        {
            case ChannelConfig_Mono:    return "Mono";
            case ChannelConfig_Stereo:  return "Stereo";
            case ChannelConfig_Quad:    return "Quad";
            case ChannelConfig_5point1: return "5.1 Surround";
            case ChannelConfig_7point1: return "7.1 Surround";
        }
        return "(unknown channel config)";
    }

    size_t framesToBytes(size_t frames, ChannelConfig config, SampleType type)
    {
        switch(config)
        {
            case ChannelConfig_Mono:    frames *= 1; break;
            case ChannelConfig_Stereo:  frames *= 2; break;
            case ChannelConfig_Quad:    frames *= 4; break;
            case ChannelConfig_5point1: frames *= 6; break;
            case ChannelConfig_7point1: frames *= 8; break;
        }
        switch(type)
        {
            case SampleType_UInt8: frames *= 1; break;
            case SampleType_Int16: frames *= 2; break;
            case SampleType_Float32: frames *= 4; break;
        }
        return frames;
    }

    size_t bytesToFrames(size_t bytes, ChannelConfig config, SampleType type)
    {
        return bytes / framesToBytes(1, config, type);
    }

    void SoundManager::clear()
    {
        SoundMap::iterator snditer = mActiveSounds.begin();
        for(;snditer != mActiveSounds.end();++snditer)
        {
            SoundBufferRefPairList::iterator sndidx = snditer->second.begin();
            for(;sndidx != snditer->second.end();++sndidx)
            {
                mOutput->stopSound(sndidx->first);
                Sound_Buffer *sfx = sndidx->second;
                if(sfx->mUses-- == 1)
                    mUnusedBuffers.push_front(sfx);
            }
        }
        mActiveSounds.clear();
        SaySoundMap::iterator sayiter = mActiveSaySounds.begin();
        for(;sayiter != mActiveSaySounds.end();++sayiter)
            mOutput->stopStream(sayiter->second.first);
        mActiveSaySounds.clear();
        TrackList::iterator trkiter = mActiveTracks.begin();
        for(;trkiter != mActiveTracks.end();++trkiter)
            mOutput->stopStream(*trkiter);
        mActiveTracks.clear();
        mPendingSaySounds.clear();
        mUnderwaterSound.reset();
        stopMusic();
    }
}
