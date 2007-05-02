/*
 *  MatroskaImportPrivate.cpp
 *
 *    MatroskaImportPrivate.cpp - C++ code for interfacing with libmatroska to import.
 *
 *
 *  Copyright (c) 2006  David Conrad
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; 
 *  version 2.1 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <QuickTime/QuickTime.h>
#include <vector>
#include <string>

#include <ebml/EbmlHead.h>
#include <ebml/EbmlSubHead.h>
#include <matroska/KaxSegment.h>
#include <matroska/KaxSeekHead.h>
#include <matroska/KaxInfo.h>
#include <matroska/KaxInfoData.h>
#include <matroska/KaxTracks.h>
#include <matroska/KaxTrackVideo.h>
#include <matroska/KaxTrackAudio.h>
#include <matroska/KaxChapters.h>
#include <matroska/KaxCluster.h>
#include <matroska/KaxBlock.h>
#include <matroska/KaxAttachments.h>
#include <matroska/KaxAttached.h>

#include "MatroskaImport.h"
#include "MatroskaCodecIDs.h"
#include "SubImport.h"
#include "CommonUtils.h"
#include "Codecprintf.h"
#include "bitstream_info.h"

using namespace std;
using namespace libmatroska;

typedef struct PixelAspectRatio {UInt32 hSpacing, vSpacing;} PixelAspectRatio;


bool MatroskaImport::OpenFile()
{
	bool valid = true;
	int upperLevel = 0;
	
	ioHandler = new DataHandlerCallback(dataRef, dataRefType, MODE_READ);
	
	aStream = new EbmlStream(*ioHandler);
	
	el_l0 = aStream->FindNextID(EbmlHead::ClassInfos, ~0);
	if (el_l0 != NULL) {
		EbmlElement *dummyElt = NULL;
		
		el_l0->Read(*aStream, EbmlHead::ClassInfos.Context, upperLevel, dummyElt, true);
		EbmlHead *head = static_cast<EbmlHead *>(el_l0);
		
		EDocType docType = GetChild<EDocType>(*head);
		if (string(docType) != "matroska") {
			Codecprintf(NULL, "Not a Matroska file\n");
			valid = false;
		}
		
		EDocTypeReadVersion readVersion = GetChild<EDocTypeReadVersion>(*head);
		if (UInt64(readVersion) > 2) {
			Codecprintf(NULL, "Matroska file too new to be read\n");
			valid = false;
		}
	} else {
		Codecprintf(NULL, "Matroska file missing EBML Head\n");
		valid = false;
	}
	
	delete el_l0;
	el_l0 = NULL;
	return valid;
}

void MatroskaImport::SetupMovie()
{
	// once we've read the Tracks and Segment Info elements and Chapters if it's in the seek head,
	// we don't need to read any more of the file
	bool done = false;
	
	el_l0 = aStream->FindNextID(KaxSegment::ClassInfos, ~0);
	if (!el_l0) return;		// nothing in the file
	
	segmentOffset = static_cast<KaxSegment *>(el_l0)->GetDataStart();
	
	SetAutoTrackAlternatesEnabled(theMovie, false);
	
	while (!done && NextLevel1Element()) {
		int upperLevel = 0;
		EbmlElement *dummyElt = NULL;
		
		if (EbmlId(*el_l1) == KaxInfo::ClassInfos.GlobalId) {
			el_l1->Read(*aStream, KaxInfo::ClassInfos.Context, upperLevel, dummyElt, true);
			ReadSegmentInfo(*static_cast<KaxInfo *>(el_l1));
			
		} else if (EbmlId(*el_l1) == KaxTracks::ClassInfos.GlobalId) {
			el_l1->Read(*aStream, KaxTracks::ClassInfos.Context, upperLevel, dummyElt, true);
			ReadTracks(*static_cast<KaxTracks *>(el_l1));
			
		} else if (EbmlId(*el_l1) == KaxChapters::ClassInfos.GlobalId) {
			el_l1->Read(*aStream, KaxChapters::ClassInfos.Context, upperLevel, dummyElt, true);
			ReadChapters(*static_cast<KaxChapters *>(el_l1));
			
		} else if (EbmlId(*el_l1) == KaxAttachments::ClassInfos.GlobalId) {
			el_l1->Read(*aStream, KaxAttachments::ClassInfos.Context, upperLevel, dummyElt, true);
			ReadAttachments(*static_cast<KaxAttachments *>(el_l1));
			
		} else if (EbmlId(*el_l1) == KaxSeekHead::ClassInfos.GlobalId) {
			el_l1->Read(*aStream, KaxSeekHead::ClassInfos.Context, upperLevel, dummyElt, true);
			ReadMetaSeek(*static_cast<KaxSeekHead *>(el_l1));
			
		} else if (EbmlId(*el_l1) == KaxCluster::ClassInfos.GlobalId) {
			// all header elements are before clusters in sane files
			done = true;
		}
	}
	
	// some final setup of info across elements since they can come in any order
	for (int i = 0; i < tracks.size(); i++) {
		tracks[i].timecodeScale = timecodeScale;
		
		// chapter tracks have to be associated with other enabled tracks to display
		if (chapterTrack) {
			AddTrackReference(tracks[i].theTrack, chapterTrack, kTrackReferenceChapterList, NULL);
		}
	}
}

EbmlElement * MatroskaImport::NextLevel1Element()
{
	int upperLevel = 0;
	
	if (el_l1) {
		el_l1->SkipData(*aStream, el_l1->Generic().Context);
		delete el_l1;
	}
	
	el_l1 = aStream->FindNextElement(el_l0->Generic().Context, upperLevel, 0xFFFFFFFFL, true);
	
	// dummy element -> probably corrupt file, search for next element in meta seek and continue from there
	if (el_l1 && el_l1->IsDummy()) {
		vector<MatroskaSeek>::iterator nextElt;
		MatroskaSeek currElt;
		currElt.segmentPos = el_l1->GetElementPosition();
		currElt.idLength = currElt.ebmlID = 0;
		
		nextElt = find_if(levelOneElements.begin(), levelOneElements.end(), bind2nd(greater<MatroskaSeek>(), currElt));
		if (nextElt != levelOneElements.end()) {
			SetContext(nextElt->GetSeekContext(segmentOffset));
			NextLevel1Element();
		}
	}
	
	return el_l1;
}

void MatroskaImport::ReadSegmentInfo(KaxInfo &segmentInfo)
{
	KaxDuration & duration = GetChild<KaxDuration>(segmentInfo);
	KaxTimecodeScale & timecodeScale = GetChild<KaxTimecodeScale>(segmentInfo);
	KaxTitle & title = GetChild<KaxTitle>(segmentInfo);
	KaxWritingApp & writingApp = GetChild<KaxWritingApp>(segmentInfo);
	
	movieDuration = Float64(duration);
	this->timecodeScale = UInt64(timecodeScale);
	SetMovieTimeScale(theMovie, S64Div(1000000000L, this->timecodeScale));
	
	QTMetaDataRef movieMetaData;
	OSStatus err = QTCopyMovieMetaData(theMovie, &movieMetaData);
	if (err == noErr) {
		OSType key;
		if (!title.IsDefaultValue()) {
			key = kQTMetaDataCommonKeyDisplayName;
			QTMetaDataAddItem(movieMetaData, 
							  kQTMetaDataStorageFormatQuickTime, kQTMetaDataKeyFormatCommon, 
							  (UInt8 *)&key, sizeof(key), 
							  (UInt8 *)UTFstring(title).GetUTF8().c_str(), 
							  UTFstring(title).GetUTF8().size(), 
							  kQTMetaDataTypeUTF8, NULL);
		}
		if (!writingApp.IsDefaultValue()) {
			key = kQTMetaDataCommonKeySoftware;
			QTMetaDataAddItem(movieMetaData, 
							  kQTMetaDataStorageFormatQuickTime, kQTMetaDataKeyFormatCommon, 
							  (UInt8 *)&key, sizeof(key), 
							  (UInt8 *)UTFstring(writingApp).GetUTF8().c_str(), 
							  UTFstring(writingApp).GetUTF8().size(), 
							  kQTMetaDataTypeUTF8, NULL);
		}
		QTMetaDataRelease(movieMetaData);
	}
}

void MatroskaImport::ReadTracks(KaxTracks &trackEntries)
{
	Track firstVideoTrack = NULL;
	Track firstAudioTrack = NULL;
	Track firstSubtitleTrack = NULL;
	
	// Since creating a subtitle track requires a video track to have already been created
    // (so that it can be sized to fit exactly over the video track), we go through the 
    // track entries in two passes, first to add audio/video, second to add subtitle tracks.
    for (int pass = 1; pass <= 2; pass++) {
		for (int i = 0; i < trackEntries.ListSize(); i++) {
			KaxTrackEntry & track = *static_cast<KaxTrackEntry *>(trackEntries[i]);
			KaxTrackNumber & number = GetChild<KaxTrackNumber>(track);
			KaxTrackType & type = GetChild<KaxTrackType>(track);
			KaxTrackDefaultDuration & defaultDuration = GetChild<KaxTrackDefaultDuration>(track);
			KaxTrackFlagDefault & enabled = GetChild<KaxTrackFlagDefault>(track);
			KaxTrackFlagLacing & lacing = GetChild<KaxTrackFlagLacing>(track);
			MatroskaTrack mkvTrack;
			
			mkvTrack.number = uint16(number);
			mkvTrack.type = uint8(type);
			mkvTrack.defaultDuration = uint32(defaultDuration) / float(timecodeScale) + .5;
			
			switch (uint8(type)) {
				case track_video:
					if (pass == 2) continue;
					if (AddVideoTrack(track, mkvTrack) != noErr) continue;
					
					if (firstVideoTrack)
						SetTrackAlternate(firstVideoTrack, mkvTrack.theTrack);
					else
						firstVideoTrack = mkvTrack.theTrack;
					break;
					
				case track_audio:
					if (pass == 2) continue;
					if (AddAudioTrack(track, mkvTrack) != noErr) continue;
					
					if (firstAudioTrack)
						SetTrackAlternate(firstAudioTrack, mkvTrack.theTrack);
					else
						firstAudioTrack = mkvTrack.theTrack;
					break;
					
				case track_subtitle:
					if (pass == 1) continue;
					if (AddSubtitleTrack(track, mkvTrack) != noErr) continue;
					
					if (firstSubtitleTrack)
						SetTrackAlternate(firstSubtitleTrack, mkvTrack.theTrack);
					else
						firstSubtitleTrack = mkvTrack.theTrack;
					break;
					
				case track_complex:
				case track_logo:
				case track_buttons:
				case track_control:
					// not likely to be implemented soon
				default:
					continue;
			}
			
			KaxTrackLanguage & trackLang = GetChild<KaxTrackLanguage>(track);
			KaxTrackName & trackName = GetChild<KaxTrackName>(track);
			
			long qtLang = ISO639_2ToQTLangCode(string(trackLang).c_str());
			SetMediaLanguage(mkvTrack.theMedia, qtLang);
			
			mkvTrack.isEnabled = uint8(enabled);
			mkvTrack.usesLacing = uint8(lacing);
			
			if (!trackName.IsDefaultValue()) {
				QTMetaDataRef trackMetaData;
				OSStatus err = QTCopyTrackMetaData(mkvTrack.theTrack, &trackMetaData);
				
				if (err == noErr) {
					OSType key = 'name';
					// QuickTime differentiates between the title of a track and its name
					// so we set both
					QTMetaDataAddItem(trackMetaData,
									  kQTMetaDataStorageFormatQuickTime,
									  kQTMetaDataKeyFormatCommon,
									  (UInt8 *)&key, sizeof(key),
									  (UInt8 *)UTFstring(trackName).GetUTF8().c_str(),
									  UTFstring(trackName).GetUTF8().size(),
									  kQTMetaDataTypeUTF8, NULL);
					
					QTMetaDataAddItem(trackMetaData,
									  kQTMetaDataStorageFormatUserData,
									  kQTMetaDataKeyFormatUserData,
									  (UInt8 *)&key, sizeof(key),
									  (UInt8 *)UTFstring(trackName).GetUTF8().c_str(),
									  UTFstring(trackName).GetUTF8().size(),
									  kQTMetaDataTypeUTF8, NULL);
					
					QTMetaDataRelease(trackMetaData);
				}
			}
			tracks.push_back(mkvTrack);
		}
	}
	
	for (int i = 0; i < tracks.size(); i++) {
		SetTrackEnabled(tracks[i].theTrack, tracks[i].isEnabled);
	}
}

ComponentResult MatroskaImport::AddVideoTrack(KaxTrackEntry &kaxTrack, MatroskaTrack &mkvTrack)
{
	ComponentResult err = noErr;
	ImageDescriptionHandle imgDesc;
	PixelAspectRatio **pasp = (PixelAspectRatio**)NewHandle(sizeof(PixelAspectRatio));
	Fixed width, height;
	
	KaxTrackVideo &videoTrack = GetChild<KaxTrackVideo>(kaxTrack);
	KaxVideoDisplayWidth & disp_width = GetChild<KaxVideoDisplayWidth>(videoTrack);
	KaxVideoDisplayHeight & disp_height = GetChild<KaxVideoDisplayHeight>(videoTrack);
	KaxVideoPixelWidth & pxl_width = GetChild<KaxVideoPixelWidth>(videoTrack);
	KaxVideoPixelHeight & pxl_height = GetChild<KaxVideoPixelHeight>(videoTrack);
	
	// Use the PixelWidth if the DisplayWidth is not set
	if (disp_width.ValueIsSet() && disp_height.ValueIsSet()) {
		// some files ignore the spec and treat display width/height as a ratio, not as pixels
		// so scale the display size to be at least as large as the pixel size here
		float horizRatio = float(uint32(pxl_width)) / uint32(disp_width);
		float vertRatio = float(uint32(pxl_height)) / uint32(disp_height);
		
		if (vertRatio > horizRatio && vertRatio > 1) {
			width = FloatToFixed(uint32(disp_width) * vertRatio);
			height = FloatToFixed(uint32(disp_height) * vertRatio);
		} else if (horizRatio > 1) {
			width = FloatToFixed(uint32(disp_width) * horizRatio);
			height = FloatToFixed(uint32(disp_height) * horizRatio);
		} else {
			width = IntToFixed(uint32(disp_width));
			height = IntToFixed(uint32(disp_height));
		}
		
		**pasp = (PixelAspectRatio){uint32(disp_width) * uint32(pxl_height),uint32(disp_height) * uint32(pxl_width)};
		if ((*pasp)->hSpacing == (*pasp)->vSpacing) **pasp = (PixelAspectRatio){1,1};
	} else if (pxl_width.ValueIsSet() && pxl_height.ValueIsSet()) {
		width = IntToFixed(uint32(pxl_width));
		height = IntToFixed(uint32(pxl_height));
		
		**pasp = (PixelAspectRatio){1,1};
	} else {
		Codecprintf(NULL, "MKV: Video has unknown dimensions.\n");
		return invalidTrack;
	}
	
	mkvTrack.theTrack = NewMovieTrack(theMovie, width, height, kNoVolume);
	if (mkvTrack.theTrack == NULL)
		return GetMoviesError();
	
	mkvTrack.theMedia = NewTrackMedia(mkvTrack.theTrack, 'vide', GetMovieTimeScale(theMovie), dataRef, dataRefType);
	if (mkvTrack.theMedia == NULL)
		return GetMoviesError();
	
	imgDesc = (ImageDescriptionHandle) NewHandleClear(sizeof(ImageDescription));
	(*imgDesc)->idSize = sizeof(ImageDescription);
    (*imgDesc)->width = uint16(pxl_width);
    (*imgDesc)->height = uint16(pxl_height);
    (*imgDesc)->frameCount = 1;
	(*imgDesc)->cType = GetFourCC(&kaxTrack);
    (*imgDesc)->depth = 24;
    (*imgDesc)->clutID = -1;
	if ((*pasp)->hSpacing != (*pasp)->vSpacing) {
		(*pasp)->hSpacing = EndianU32_NtoB((*pasp)->hSpacing);
		(*pasp)->vSpacing = EndianU32_NtoB((*pasp)->vSpacing);
		AddImageDescriptionExtension(imgDesc,(Handle)pasp,kICMImageDescriptionPropertyID_PixelAspectRatio);
	}
	
	mkvTrack.desc = (SampleDescriptionHandle) imgDesc;
	
	// this sets up anything else needed in the description for the specific codec.
	err = MkvFinishSampleDescription(&kaxTrack, (SampleDescriptionHandle) imgDesc, kToSampleDescription);
	if (err) return err;
	
	// video tracks can have display offsets, so create a sample table
	err = QTSampleTableCreateMutable(NULL, GetMovieTimeScale(theMovie), NULL, &mkvTrack.sampleTable);
	if (err) return err;
	
	err = QTSampleTableAddSampleDescription(mkvTrack.sampleTable, mkvTrack.desc, 0, &mkvTrack.qtSampleDesc);
	return err;
}

ComponentResult MatroskaImport::AddAudioTrack(KaxTrackEntry &kaxTrack, MatroskaTrack &mkvTrack)
{
	SoundDescriptionHandle sndDesc;
	AudioStreamBasicDescription asbd = {0};
	AudioChannelLayout acl;
    AudioChannelLayout *pacl = &acl;
    ByteCount acl_size = sizeof(acl);
	ByteCount ioSize = sizeof(asbd);
	ByteCount cookieSize = 0;
	Ptr magicCookie = NULL;
	
	mkvTrack.theTrack = NewMovieTrack(theMovie, 0, 0, kFullVolume);
	if (mkvTrack.theTrack == NULL)
		return GetMoviesError();
	
	mkvTrack.theMedia = NewTrackMedia(mkvTrack.theTrack, 'soun', GetMovieTimeScale(theMovie), dataRef, dataRefType);
	if (mkvTrack.theMedia == NULL)
		return GetMoviesError();
	
	KaxTrackAudio & audioTrack = GetChild<KaxTrackAudio>(kaxTrack);
	KaxAudioSamplingFreq & sampleFreq = GetChild<KaxAudioSamplingFreq>(audioTrack);
	KaxAudioChannels & numChannels = GetChild<KaxAudioChannels>(audioTrack);
	KaxAudioBitDepth & bitDepth = GetChild<KaxAudioBitDepth>(audioTrack);
	
	asbd.mBitsPerChannel = uint32(bitDepth);
	asbd.mFormatID = GetFourCC(&kaxTrack);
	asbd.mSampleRate = Float64(sampleFreq);
	asbd.mChannelsPerFrame = uint32(numChannels);
	asbd.mFramesPerPacket = 1;		// needed for mp3 and v1 SoundDescription, maybe others
	
	MkvFinishASBD(&kaxTrack, &asbd);
	
	// get more info about the codec
	AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, NULL, &ioSize, &asbd);
	
	acl = GetDefaultChannelLayout(&asbd);
	
	if (acl.mChannelLayoutTag == 0) {
		pacl = NULL;
		acl_size = 0;
	}
	
	OSStatus err = QTSoundDescriptionCreate(&asbd, pacl, acl_size, magicCookie, cookieSize, 
											kQTSoundDescriptionKind_Movie_LowestPossibleVersion, &sndDesc);
	if (err) return err;
	
	mkvTrack.desc = (SampleDescriptionHandle) sndDesc;
	return MkvFinishSampleDescription(&kaxTrack, mkvTrack.desc, kToSampleDescription);
}

ComponentResult MatroskaImport::AddSubtitleTrack(KaxTrackEntry &kaxTrack, MatroskaTrack &mkvTrack)
{
	Fixed trackWidth, trackHeight;
	Rect movieBox;
	MediaHandler mh;
	ImageDescriptionHandle imgDesc = (ImageDescriptionHandle) NewHandleClear(sizeof(ImageDescription));
	mkvTrack.desc = (SampleDescriptionHandle) imgDesc;
	
	// we assume that a video track has already been created, so we can set the subtitle track
	// to have the same dimensions as it. Note that this doesn't work so well with multiple
	// video tracks with different dimensions; but QuickTime doesn't expect that; how should we handle them?
	GetMovieBox(theMovie, &movieBox);
	trackWidth = IntToFixed(movieBox.right - movieBox.left);
	trackHeight = IntToFixed(movieBox.bottom - movieBox.top);
	
	(*imgDesc)->idSize = sizeof(ImageDescription);
	(*imgDesc)->cType = GetFourCC(&kaxTrack);
	(*imgDesc)->frameCount = 1;
	(*imgDesc)->depth = 32;
	(*imgDesc)->clutID = -1;
	
	if ((*imgDesc)->cType == kSubFormatVobSub) {
		int width, height;
		
		// bitmap width & height is in the codec private in text format
		KaxCodecPrivate & idxFile = GetChild<KaxCodecPrivate>(kaxTrack);
		string idxFileStr((char *)idxFile.GetBuffer(), idxFile.GetSize());
		string::size_type loc = idxFileStr.find("size:", 0);
		if (loc == string::npos)
			return -1;
		
		sscanf(&idxFileStr.c_str()[loc], "size: %dx%d", &width, &height);
		(*imgDesc)->width = width;
		(*imgDesc)->height = height;
		
		if (trackWidth == 0 || trackHeight == 0) {
			trackWidth = IntToFixed(width);
			trackHeight = IntToFixed(height);
		}
		
		mkvTrack.theTrack = NewMovieTrack(theMovie, trackWidth, trackHeight, kNoVolume);
		if (mkvTrack.theTrack == NULL)
			return GetMoviesError();
		
		mkvTrack.theMedia = NewTrackMedia(mkvTrack.theTrack, 'vide', GetMovieTimeScale(theMovie), dataRef, dataRefType);
		if (mkvTrack.theMedia == NULL)
			return GetMoviesError();
		
		// finally, say that we're transparent
		mh = GetMediaHandler(mkvTrack.theMedia);
		MediaSetGraphicsMode(mh, graphicsModePreBlackAlpha, NULL);
		
		// subtitle tracks should be above the video track, which should be layer 0
		SetTrackLayer(mkvTrack.theTrack, -1);
		
		mkvTrack.is_vobsub = true;
		
	} else if ((*imgDesc)->cType == kSubFormatUTF8 || (*imgDesc)->cType == kSubFormatSSA || (*imgDesc)->cType == kSubFormatASS) {
		if ((*imgDesc)->cType == kSubFormatASS) (*imgDesc)->cType = kSubFormatSSA; // no real reason to treat them differently
		UInt32 emptyDataRefExtension[2]; // XXX the various uses of this bit of code should be unified
		mkvTrack.subDataRefHandler = NewHandleClear(sizeof(Handle) + 1);
		emptyDataRefExtension[0] = EndianU32_NtoB(sizeof(UInt32)*2);
		emptyDataRefExtension[1] = EndianU32_NtoB(kDataRefExtensionInitializationData);
		
		PtrAndHand(&emptyDataRefExtension[0], mkvTrack.subDataRefHandler, sizeof(emptyDataRefExtension));
		
		mkvTrack.theTrack = CreatePlaintextSubTrack(theMovie, imgDesc, GetMovieTimeScale(theMovie), mkvTrack.subDataRefHandler, HandleDataHandlerSubType, (*imgDesc)->cType, NULL, movieBox);
		if (mkvTrack.theTrack == NULL)
			return GetMoviesError();
		
		mkvTrack.theMedia = GetTrackMedia(mkvTrack.theTrack);
		mh = GetMediaHandler(mkvTrack.theMedia);
		MediaSetGraphicsMode(mh, graphicsModePreBlackAlpha, NULL);
		
		BeginMediaEdits(mkvTrack.theMedia);
		
		mkvTrack.is_vobsub = false;
	} else {
		Codecprintf(NULL, "MKV: Unsupported subtitle type\n");
		return -2;
	}
	
	// this sets up anything else needed in the description for the specific codec.
	return MkvFinishSampleDescription(&kaxTrack, (SampleDescriptionHandle) imgDesc, kToSampleDescription);
}

void MatroskaImport::ReadChapters(KaxChapters &chapterEntries)
{
	KaxEditionEntry & edition = GetChild<KaxEditionEntry>(chapterEntries);
	UInt32 emptyDataRefExtension[2];
	
	chapterTrack = NewMovieTrack(theMovie, 0, 0, kNoVolume);
	if (chapterTrack == NULL) {
		Codecprintf(NULL, "MKV: Error creating chapter track %d\n", GetMoviesError());
		return;
	}
	
	// we use a handle data reference here because I don't see any way to add textual 
	// sample references (TextMediaAddTextSample() will behave the same as AddSample()
	// in that it modifies the original file if that's the data reference of the media)
	Handle dataRef = NewHandleClear(sizeof(Handle) + 1);

	emptyDataRefExtension[0] = EndianU32_NtoB(sizeof(UInt32)*2);
	emptyDataRefExtension[1] = EndianU32_NtoB(kDataRefExtensionInitializationData);
	
	PtrAndHand(&emptyDataRefExtension[0], dataRef, sizeof(emptyDataRefExtension));
	
	Media chapterMedia = NewTrackMedia(chapterTrack, TextMediaType, GetMovieTimeScale(theMovie), 
									   dataRef, HandleDataHandlerSubType);
	if (chapterMedia == NULL) {
		Codecprintf(NULL, "MKV: Error creating chapter media %d\n", GetMoviesError());
		return;
	}
	
	// Name the chapter track "Chapters" for easy distinguishing
	QTMetaDataRef trackMetaData;
	OSErr err = QTCopyTrackMetaData(chapterTrack, &trackMetaData);
	if (err == noErr) {
		OSType key = kUserDataName;
		string chapterName("Chapters");
		QTMetaDataAddItem(trackMetaData, kQTMetaDataStorageFormatUserData,
						  kQTMetaDataKeyFormatUserData, (UInt8 *)&key, sizeof(key),
						  (UInt8 *)chapterName.c_str(), chapterName.size(),
						  kQTMetaDataTypeUTF8, NULL);
		QTMetaDataRelease(trackMetaData);
	}
	
	BeginMediaEdits(chapterMedia);
	
	// tell the text media handler the upcoming text samples are
	// encoded in Unicode with a byte order mark (BOM)
	MediaHandler mediaHandler = GetMediaHandler(chapterMedia);
	SInt32 dataPtr = kTextEncodingUnicodeDefault;
	TextMediaSetTextSampleData(mediaHandler, &dataPtr, kTXNTextEncodingAttribute);
	
	KaxChapterAtom *chapterAtom = FindChild<KaxChapterAtom>(edition);
	while (chapterAtom && chapterAtom->GetSize() > 0) {
		AddChapterAtom(chapterAtom, chapterTrack);
		chapterAtom = &GetNextChild<KaxChapterAtom>(edition, *chapterAtom);
	}
	
	EndMediaEdits(chapterMedia);
	SetTrackEnabled(chapterTrack, false);
}

void MatroskaImport::AddChapterAtom(KaxChapterAtom *atom, Track chapterTrack)
{
	KaxChapterAtom *subChapter = FindChild<KaxChapterAtom>(*atom);
	bool addThisChapter = true;
	
	// since QuickTime only supports linear chapter tracks (no nesting), only add chapter leaves
	if (subChapter && subChapter->GetSize() > 0) {
		while (subChapter && subChapter->GetSize() > 0) {
			KaxChapterFlagHidden &hideChapter = GetChild<KaxChapterFlagHidden>(*subChapter);
			
			if (!uint8_t(hideChapter)) {
				AddChapterAtom(subChapter, chapterTrack);
				addThisChapter = false;
			}
			subChapter = &GetNextChild(*atom, *subChapter);
		}
	} 
	if (addThisChapter) {
		// add the chapter to the track if it has no children
		KaxChapterTimeStart & startTime = GetChild<KaxChapterTimeStart>(*atom);
		KaxChapterDisplay & chapDisplay = GetChild<KaxChapterDisplay>(*atom);
		KaxChapterString & chapString = GetChild<KaxChapterString>(chapDisplay);
		MediaHandler mh = GetMediaHandler(GetTrackMedia(chapterTrack));
		
		Rect bounds = {0, 0, 0, 0};
		TimeValue inserted;
		OSErr err = TextMediaAddTextSample(mh, 
										   const_cast<Ptr>(UTFstring(chapString).GetUTF8().c_str()), 
										   UTFstring(chapString).GetUTF8().size(), 
										   0, 0, 0, NULL, NULL, 
										   teCenter, &bounds, dfClipToTextBox, 
										   0, 0, 0, NULL, 1, &inserted);
		if (err)
			Codecprintf(NULL, "MKV: Error adding text sample %d\n", err);
		else {
			TimeValue start = UInt64(startTime) / timecodeScale;
			
			InsertMediaIntoTrack(chapterTrack, start, inserted, 1, fixed1);
		}
	}
}

void MatroskaImport::ReadAttachments(KaxAttachments &attachments)
{
	KaxAttached *attachedFile = FindChild<KaxAttached>(attachments);
	
	while (attachedFile && attachedFile->GetSize() > 0) {
		string fileMimeType = GetChild<KaxMimeType>(*attachedFile);
		/* The only attachments handled here are fonts, which currently can be truetype or opentype.
		   application/x-* is probably not a permanent MIME type, but it is current practice... */
		if (fileMimeType == "application/x-truetype-font" || fileMimeType == "application/x-font-otf") {
			KaxFileData & fontData = GetChild<KaxFileData>(*attachedFile);
			
			if (fontData.GetSize()) {
				ATSFontContainerRef container;
				ATSFontActivateFromMemory(fontData.GetBuffer(), fontData.GetSize(), kATSFontContextLocal, 
				                          kATSFontFormatUnspecified, NULL, kATSOptionFlagsDefault, &container);
			}
		}
		
		attachedFile = &GetNextChild<KaxAttached>(attachments, *attachedFile);
	}
}

void MatroskaImport::ReadMetaSeek(KaxSeekHead &seekHead)
{
	KaxSeek *seekEntry = FindChild<KaxSeek>(seekHead);
	
	// don't re-read a seek head that's already been read
	uint64_t currPos = seekHead.GetElementPosition();
	vector<MatroskaSeek>::iterator itr = levelOneElements.begin();
	for (; itr != levelOneElements.end(); itr++) {
		if (itr->GetID() == KaxSeekHead::ClassInfos.GlobalId && 
			itr->segmentPos + segmentOffset == currPos)
			return;
	}
	
	while (seekEntry && seekEntry->GetSize() > 0) {
		MatroskaSeek newSeekEntry;
		KaxSeekID & seekID = GetChild<KaxSeekID>(*seekEntry);
		KaxSeekPosition & position = GetChild<KaxSeekPosition>(*seekEntry);
		EbmlId elementID = EbmlId(seekID.GetBuffer(), seekID.GetSize());
		
		newSeekEntry.ebmlID = elementID.Value;
		newSeekEntry.idLength = elementID.Length;
		newSeekEntry.segmentPos = position;
		
		// recursively read seek heads that are pointed to by the current one
		if (elementID == KaxSeekHead::ClassInfos.GlobalId) {
			MatroskaSeekContext savedContext = SaveContext();
			SetContext(newSeekEntry.GetSeekContext(segmentOffset));
			
			if (NextLevel1Element() && EbmlId(*el_l1) == KaxSeekHead::ClassInfos.GlobalId) {
				int upperLevel = 0;
				EbmlElement *dummyElt = NULL;
				el_l1->Read(*aStream, KaxSeekHead::ClassInfos.Context, upperLevel, dummyElt, true);
				ReadMetaSeek(*static_cast<KaxSeekHead *>(el_l1));
			}
			SetContext(savedContext);
		}
		
		levelOneElements.push_back(newSeekEntry);
		seekEntry = &GetNextChild<KaxSeek>(seekHead, *seekEntry);
	}
	
	sort(levelOneElements.begin(), levelOneElements.end());
}

void MatroskaImport::ImportCluster(KaxCluster &cluster, bool addToTrack)
{
	KaxSegment & segment = *static_cast<KaxSegment *>(el_l0);
	KaxClusterTimecode & clusterTime = GetChild<KaxClusterTimecode>(cluster);
	CXXAutoreleasePool pool;
				
	cluster.SetParent(segment);
	cluster.InitTimecode(uint64(clusterTime), timecodeScale);
	
	for (int i = 0; i < cluster.ListSize(); i++) {
		const EbmlId & elementID = EbmlId(*cluster[i]);
		KaxInternalBlock *block = NULL;
		uint32_t duration = 0;		// set to track's default duration in AddBlock if 0
		short flags = 0;
		
		if (elementID == KaxBlockGroup::ClassInfos.GlobalId) {
			KaxBlockGroup & blockGroup = *static_cast<KaxBlockGroup *>(cluster[i]);
			KaxBlockDuration & blkDuration = GetChild<KaxBlockDuration>(blockGroup);
			block = &GetChild<KaxBlock>(blockGroup);
			if (blkDuration.ValueIsSet())
				duration = uint32(blkDuration);
			flags = blockGroup.ReferenceCount() > 0 ? mediaSampleNotSync : 0;
			
		} else if (elementID == KaxSimpleBlock::ClassInfos.GlobalId) {
			KaxSimpleBlock & simpleBlock = *static_cast<KaxSimpleBlock *>(cluster[i]);
			block = &simpleBlock;
			if (!simpleBlock.IsKeyframe())
				flags |= mediaSampleNotSync;
			if (simpleBlock.IsDiscardable())
				flags |= mediaSampleDroppable;
		}
		
		if (block) {
			block->SetParent(cluster);
			
			for (int i = 0; i < tracks.size(); i++) {
				if (tracks[i].number == block->TrackNum()) {
					tracks[i].AddBlock(*block, duration, flags);
					break;
				}
			}
		}
	}
	
	if (addToTrack) {
		for (int i = 0; i < tracks.size(); i++)
			tracks[i].AddSamplesToTrack();
		
		loadState = kMovieLoadStatePlayable;
	}
}

MatroskaSeekContext MatroskaImport::SaveContext()
{
	MatroskaSeekContext ret = { el_l1, ioHandler->getFilePointer() };
	el_l1 = NULL;
	return ret;
}

void MatroskaImport::SetContext(MatroskaSeekContext context)
{
	if (el_l1)
		delete el_l1;
	
	el_l1 = context.el_l1;
	ioHandler->setFilePointer(context.position);
}


MatroskaTrack::MatroskaTrack()
{
	number = 0;
	type = -1;
	theTrack = NULL;
	theMedia = NULL;
	desc = NULL;
	sampleTable = NULL;
	qtSampleDesc = 0;
	timecodeScale = 1000000;
	maxLoadedTime = 0;
	seenFirstBlock = false;
	firstSample = -1;
	numSamples = 0;
	durationToAdd = 0;
	displayOffsetSum = 0;
	durationSinceZeroSum = 0;
	subtitleSerializer = new CXXSubtitleSerializer;
	subDataRefHandler = NULL;
	is_vobsub = false;
	isEnabled = true;
	defaultDuration = 0;
	usesLacing = true;
	currentFrame = 0;
}

MatroskaTrack::MatroskaTrack(const MatroskaTrack &copy)
{
	number = copy.number;
	type = copy.type;
	theTrack = copy.theTrack;
	theMedia = copy.theMedia;
	
	if (copy.desc) {
		desc = (SampleDescriptionHandle) NewHandle((*copy.desc)->descSize);
		memcpy(*desc, *copy.desc, (*copy.desc)->descSize);
	} else
		desc = NULL;
	
	sampleTable = copy.sampleTable;
	if (sampleTable)
		QTSampleTableRetain(sampleTable);
	
	qtSampleDesc = copy.qtSampleDesc;
	timecodeScale = copy.timecodeScale;
	maxLoadedTime = copy.maxLoadedTime;
	
	for (int i = 0; i < copy.lastFrames.size(); i++)
		lastFrames.push_back(copy.lastFrames[i]);
	
	seenFirstBlock = copy.seenFirstBlock;
	firstSample = copy.firstSample;
	numSamples = copy.numSamples;
	durationToAdd = copy.durationToAdd;
	displayOffsetSum = copy.displayOffsetSum;
	durationSinceZeroSum = copy.durationSinceZeroSum;
	
	subtitleSerializer = copy.subtitleSerializer;
	subtitleSerializer->retain();
		
	subDataRefHandler = copy.subDataRefHandler;
	
	is_vobsub = copy.is_vobsub;
	isEnabled = copy.isEnabled;
	defaultDuration = copy.defaultDuration;
	usesLacing = copy.usesLacing;
	currentFrame = copy.currentFrame;
}

MatroskaTrack::~MatroskaTrack()
{
	if (desc)
		DisposeHandle((Handle) desc);
	
	if (sampleTable)
		QTSampleTableRelease(sampleTable);
	
	if (subtitleSerializer)
		subtitleSerializer->release();
}

void MatroskaTrack::ParseFirstBlock(KaxInternalBlock &block)
{
	AudioStreamBasicDescription asbd = {0};
	AudioChannelLayout acl = {0};
	bool replaceSoundDesc = false;
	
	switch ((*desc)->dataFormat) {
		case kAudioFormatAC3:
			replaceSoundDesc = parse_ac3_bitstream(&asbd, &acl, block.GetBuffer(0).Buffer(), block.GetFrameSize(0));
			break;
	}
	
	if (replaceSoundDesc) {
		// successful in parsing, so the acl and asbd are more correct than what we generated in 
		// AddAudioTrack() so replace our sound description
		SoundDescriptionHandle sndDesc = NULL;
		
		OSStatus err = QTSoundDescriptionCreate(&asbd, &acl, sizeof(AudioChannelLayout), NULL, 0, 
		                                        kQTSoundDescriptionKind_Movie_LowestPossibleVersion, &sndDesc);
		if (err == noErr) {
			DisposeHandle((Handle) desc);
			desc = (SampleDescriptionHandle) sndDesc;
		}
	}
}

void MatroskaTrack::AddBlock(KaxInternalBlock &block, uint32 duration, short flags)
{
	if (!seenFirstBlock) {
		ParseFirstBlock(block);
		seenFirstBlock = true;
	}
	
	// tracks w/ lacing can't have b-frames, and neither can any known audio codec
	if (usesLacing || type != track_video) {
		// don't add the blocks until we get one with a new timecode
		TimeValue64 duration = 0;
		if (lastFrames.size() > 0)
			duration = block.GlobalTimecode() / timecodeScale - lastFrames[0].pts;
		
		if (duration > 0) {
			for (int i = 0; i < lastFrames.size(); i++) {
				// since there can be multiple frames in one block, split the duration evenly between them
				// giving the remainder to the latter blocks
				int remainder = duration % lastFrames.size() >= lastFrames.size() - i ? 1 : 0;
				
				lastFrames[i].duration = duration / lastFrames.size() + remainder;
				
				AddFrame(lastFrames[i]);
			}
			lastFrames.clear();
		}
	} else if (ptsReorder.size() - currentFrame > MAX_DECODE_DELAY + 1) {
		map<TimeValue64, TimeValue>::iterator duration;
		MatroskaFrame &curr = lastFrames[currentFrame];
		MatroskaFrame &next = lastFrames[currentFrame+1];
		currentFrame++;
		
		// pts -> dts works this way: we assume that the first frame has correct dts 
		// (we start at a keyframe, so pts = dts), and then we fill up a buffer with
		// frames until we have the frame whose pts is equal to the next dts
		// Then, we sort this buffer, extract the pts as dts, and calculate the duration.
		
		ptsReorder.sort();
		next.dts = *(++ptsReorder.begin());
		ptsReorder.pop_front();
		
		// Duration calculation has to be done between consecutive pts. Since we reorder
		// the pts into the dts, which have to be in order, we calculate the duration then
		// from the dts, then save it for the frame with the same pts.
		
		durationForPTS[curr.dts] = next.dts - curr.dts;
		
		duration = durationForPTS.find(lastFrames[0].pts);
		if (duration != durationForPTS.end()) {
			lastFrames[0].duration = duration->second;
			AddFrame(lastFrames[0]);
			lastFrames.erase(lastFrames.begin());
			durationForPTS.erase(duration);
			currentFrame--;
		}
	}
	
	for (int i = 0; i < block.NumberFrames(); i++) {
		MatroskaFrame newFrame;
		newFrame.pts = block.GlobalTimecode() / timecodeScale;
		newFrame.dts = newFrame.pts;
		if (duration > 0)
			newFrame.duration = duration;
		else
			newFrame.duration = defaultDuration;
		newFrame.offset = block.GetDataPosition(i);
		newFrame.size = block.GetFrameSize(i);
		newFrame.flags = flags;

		if (type == track_subtitle) {
			newFrame.buffer = &block.GetBuffer(i);
			AddFrame(newFrame);
		}
		else {
			lastFrames.push_back(newFrame);
			if (!usesLacing && type == track_video)
				ptsReorder.push_back(newFrame.pts);
		}
		
		newFrame.buffer = NULL;
	}
}

void MatroskaTrack::AddFrame(MatroskaFrame &frame)
{
	ComponentResult err = noErr;
	TimeValue sampleTime;
	TimeValue64 displayOffset = frame.pts - frame.dts;
	
	if (type == track_subtitle && !is_vobsub) {
		if (frame.size > 0) subtitleSerializer->pushLine((const char*)frame.buffer->Buffer(), frame.buffer->Size(), frame.pts, frame.pts + frame.duration);
		const char *packet=NULL; size_t size=0; unsigned start=0, end=0;
		packet = subtitleSerializer->popPacket(&size, &start, &end);
		if (packet) {
			Handle sampleH;
			PtrToHand(packet, &sampleH, size);
			err = AddMediaSample(theMedia, sampleH, 0, size, end - start, desc, 1, 0, &sampleTime);
			if (err) {
				Codecprintf(NULL, "MKV: error adding subtitle sample %d\n", err);
				return;
			}
			DisposeHandle(sampleH);
			frame.pts = start;
			frame.duration = end - start;
		} else return;
	} else if (sampleTable) {
		SInt64 sampleNum;
		
		err = QTSampleTableAddSampleReferences(sampleTable, frame.offset, frame.size, frame.duration, 
											   displayOffset, 1, frame.flags, qtSampleDesc, &sampleNum);
		if (err) {
			Codecprintf(NULL, "MKV: error adding sample reference to table %d\n", err);
			return;
		}
		
		if (firstSample == -1)
			firstSample = sampleNum;
		numSamples++;
	} else {
		SampleReference64Record sample;
		sample.dataOffset = SInt64ToWide(frame.offset);
		sample.dataSize = frame.size;
		sample.durationPerSample = frame.duration;
		sample.numberOfSamples = 1;
		sample.sampleFlags = frame.flags;
		
		err = AddMediaSampleReferences64(theMedia, desc, 1, &sample, &sampleTime);
		if (err) {
			Codecprintf(NULL, "MKV: error adding sample reference to media %d\n", err);
			return;
		}
	}
	
	// add to track immediately if subtitle, otherwise we let it be added elsewhere when we can do several at once
	if (type == track_subtitle) {
		err = InsertMediaIntoTrack(theTrack, frame.pts, sampleTime, frame.duration, fixed1);
		if (err) {
			Codecprintf(NULL, "MKV: error adding subtitle media into track %d\n", err);
			return;
		}
	} else {
		durationSinceZeroSum += frame.duration;
		displayOffsetSum += displayOffset;
		if (displayOffsetSum == 0) {
			durationToAdd += durationSinceZeroSum;
			durationSinceZeroSum = 0;
		}
	}
}

void MatroskaTrack::AddSamplesToTrack()
{
	OSStatus err = noErr;
	
	if (type == track_subtitle)
		return;			// handled in AddFrame()

	if (sampleTable) {
		if (firstSample == -1)
			return;		// nothing to add
		
		err = AddSampleTableToMedia(theMedia, sampleTable, firstSample, numSamples, NULL);
		firstSample = -1;
		numSamples = 0;
		if (err) {
			Codecprintf(NULL, "MKV: error adding sample table to the media %d\n", err);
			durationToAdd = 0;
			return;
		}
	}
	
	err = InsertMediaIntoTrack(theTrack, -1, maxLoadedTime, durationToAdd, fixed1);
	if (err)
		Codecprintf(NULL, "MKV: error inserting media into track %d\n", err);
	else
		maxLoadedTime += durationToAdd;
	
	durationToAdd = 0;
}

void MatroskaTrack::FinishTrack()
{
	
	if (type == track_subtitle && !is_vobsub)
	{
		 subtitleSerializer->setFinished();
		 do {
			 MatroskaFrame fr = {0};
			 AddFrame(fr); // add empty frames to flush the subtitle packet queue
		 } while (!subtitleSerializer->empty());
		 EndMediaEdits(theMedia);
	}
	
	AddSamplesToTrack();
}
