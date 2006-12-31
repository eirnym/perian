/*****************************************************************************
 *
 *  Avi Import Component Dispatch Header
 *
 *  Copyright(C) 2006 Christoph Naegeli <chn1@mac.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *  
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *  
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 ****************************************************************************/

	ComponentSelectorOffset (-kComponentTargetSelect)

	ComponentRangeCount (1)
	ComponentRangeShift (7)
	ComponentRangeMask	(7F)
	
//	ComponentRangeBegin	(4)
	ComponentRangeBegin	(0)
		ComponentError	(Target)
		ComponentError  (Register)
		StdComponentCall(Version)
		StdComponentCall(CanDo)
		StdComponentCall(Close)
		StdComponentCall(Open)
	ComponentRangeEnd	(0)
	
	ComponentRangeBegin	(1)
		ComponentError	(0)
		ComponentError	(Handle)
		ComponentCall	(File)
		ComponentError	(SetSampleDuration)
		ComponentCall	(SetSampleDescription)
		ComponentError	(SetMediaFile)
		ComponentError	(SetDimensions)
		ComponentError	(SetChunkSize)
		ComponentCall	(SetProgressProc)
		ComponentError	(SetAuxiliaryData)
		ComponentError	(SetFromScrap)
		ComponentError	(DoUserDialog)
		ComponentError	(SetDuration)
		ComponentError	(GetAuxiliaryDataType)
		ComponentCall	(Validate)
		ComponentError	(GetFileType)
		ComponentCall	(DataRef)
		ComponentError	(GetSampleDescription)
		ComponentCall	(GetMIMETypeList)
		ComponentError	(SetOffsetAndLimit)
		ComponentError	(GetSettingsAsAtomContainer)
		ComponentError	(SetSettingsFromAtomContainer)
		ComponentError	(SetOffsetAndLimit64)
		ComponentCall	(Idle)
		ComponentCall	(ValidateDataRef)
		ComponentCall	(GetLoadState)
		ComponentCall	(GetMaxLoadedTime)
		ComponentError	(EstimateCompletionTime)
		ComponentError	(SetDontBlock)
		ComponentError	(GetDontBlock)
		ComponentCall	(SetIdleManager)
		ComponentError	(SetNewMovieFlags)
		ComponentCall	(GetDestinationMediaType)
		ComponentError	(SetMediaDataRef)
		ComponentError	(DoUserDialogDataRef)
	ComponentRangeEnd	(1)
