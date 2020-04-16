/*
	todo(jax): THIS IS NOT A FINAL PLATFORM LAYER!!!

	- Saved game locations
	- Getting a handle to our own executable files
	- Asset loading path
	- Threading (launch a thread)
	- Raw Input (support multiple keyboards & finer mouse movements)
	- Sleep / timeBeginPeriod
	- ClipCursor() (multi-monitor support)
	- Fullscreen support
	- WM_SETCURSOR (control cursor visibility)
	- QueryCancelAutoplay
	- WM_ACTIVATEAPP (for when we are not the active app)
	- Blit speed improvements (BitBlit)
	- Hardware acceleration (OpenGL or Direct3D or BOTH???)
	- GetKeyboardLayout (for French kbs, international WASD support)

	Just a partial list of stuff!!!
*/

#include "handmade.h"

#include <malloc.h>
#include <windows.h>
#include <stdio.h> // for sprintf
#include <xinput.h>
#include <dsound.h>

#include "win32_handmade.h"

global_variable bool GlobalRunning;
global_variable bool GlobalPause;
global_variable win32_offscreen_buffer GlobalBackbuffer;
global_variable LPDIRECTSOUNDBUFFER GlobalSecondaryBuffer;
global_variable uint64 GlobalPerfCountFrequency;

#define XINPUTGETSTATE(name) DWORD WINAPI name(DWORD dwUserIndex, XINPUT_STATE* pState)
typedef XINPUTGETSTATE(x_input_get_state);

XINPUTGETSTATE(XInputGetStateStub) {
	return ERROR_DEVICE_NOT_CONNECTED;
}
global_variable x_input_get_state* XInputGetState_ = XInputGetStateStub;
#define XInputGetState XInputGetState_

#define XINPUTSETSTATE(name) DWORD WINAPI name(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration)
typedef XINPUTSETSTATE(x_input_set_state);

XINPUTSETSTATE(XInputSetStateStub) {
	return ERROR_DEVICE_NOT_CONNECTED;
}

global_variable x_input_set_state* XInputSetState_ = XInputSetStateStub;
#define XInputSetState XInputSetState_

#define DIRECTSOUNDCREATE(name) HRESULT WINAPI name(LPCGUID pcGuidDevice, LPDIRECTSOUND* ppDS, LPUNKNOWN pUnkOuter);
typedef DIRECTSOUNDCREATE(direct_sound_create);

DEBUG_PLATFORM_FREE_FILE_MEMORY(DEBUGPlatformFreeFileMemory) {
	if (Memory) {
		VirtualFree(Memory, 0, MEM_RELEASE);
	}
}

DEBUG_PLATFORM_READ_ENTIRE_FILE(DEBUGPlatformReadEntireFile) {
	debug_read_file_result Result = {};

	HANDLE FileHandle = CreateFileA(Filename, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
	if (FileHandle != INVALID_HANDLE_VALUE) {
		LARGE_INTEGER FileSize;
		if (GetFileSizeEx(FileHandle, &FileSize)) {
			// note(jax): Windows can't read files greater than 4GB via ReadFile. Since this
			// is debug code, we can assure that we won't be reading files that large here.
			// Assert(FileSize.QuadPart <= 0xFFFFFFFF);
			uint32 FileSize32 = SafeTruncateUInt64(FileSize.QuadPart);
			Result.Contents = VirtualAlloc(0, FileSize32, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
			if (Result.Contents) {
				DWORD BytesRead;
				if (ReadFile(FileHandle, Result.Contents, FileSize32, &BytesRead, 0) && (FileSize32 == BytesRead)) {
					// note(jax): File read successfully
					Result.ContentsSize = FileSize32;
				} else { // note(jax): Allocation failure!!!
					DEBUGPlatformFreeFileMemory(Result.Contents);
					Result.Contents = 0;
				}
			} else {
				// todo(jax): Logging
			}
		} else {
			// todo(jax): Logging
		}

		CloseHandle(FileHandle);
	} else {
		// todo(jax): Logging
	}

	return Result;
}

DEBUG_PLATFORM_WRITE_ENTIRE_FILE(DEBUGPlatformWriteEntireFile) {
	bool32 Result = false;

	HANDLE FileHandle = CreateFileA(Filename, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
	if (FileHandle != INVALID_HANDLE_VALUE) {
		DWORD BytesWritten;
		if (WriteFile(FileHandle, Memory, MemorySize, &BytesWritten, 0)) {
			// note(jax): File read successfully
			Result = (BytesWritten == MemorySize);
		} else { // note(jax): Allocation failure!!!

		}

		CloseHandle(FileHandle);
	} else {
		// todo(jax): Logging
	}

	return Result;
}

struct win32_game_code {
	HMODULE GameCodeDLL;
	FILETIME DLLLastWriteTime;
	game_update_and_render* UpdateAndRender;
	game_get_sound_samples* GetSoundSamples;

	bool32 IsValid;
};

inline FILETIME Win32GetLastWriteTime(char* Filename) {
	FILETIME LastWriteTime = {};

	WIN32_FIND_DATA FindData;
	HANDLE FindHandle = FindFirstFileA(Filename, &FindData);
	if (FindHandle != INVALID_HANDLE_VALUE) {
		LastWriteTime = FindData.ftLastWriteTime;
		FindClose(FindHandle);
	}

	return LastWriteTime;
}

internal win32_game_code Win32LoadGameCode(char* SourceDLLName, char* TempDLLName) {
	win32_game_code Result = {};

	// todo(jax): Need to get the proper path here!
	// todo(jax): Automatic determination of when updates are necessary.

	Result.DLLLastWriteTime = Win32GetLastWriteTime(SourceDLLName);

	CopyFileA(SourceDLLName, TempDLLName, FALSE);

	Result.GameCodeDLL = LoadLibraryA(TempDLLName);
	if (Result.GameCodeDLL) {
		Result.UpdateAndRender = (game_update_and_render*) GetProcAddress(Result.GameCodeDLL, "GameUpdateAndRender");
		Result.GetSoundSamples = (game_get_sound_samples*) GetProcAddress(Result.GameCodeDLL, "GameGetSoundSamples");

		Result.IsValid = (Result.UpdateAndRender && Result.GetSoundSamples);
	}

	if (!Result.IsValid) {
		Result.UpdateAndRender = GameUpdateAndRenderStub;
		Result.GetSoundSamples = GameGetSoundSamplesStub;
	}

	return Result;
}

internal void Win32UnloadGameCode(win32_game_code* Game) {
	if (Game->GameCodeDLL) {
		FreeLibrary(Game->GameCodeDLL);
		Game->GameCodeDLL = 0;
	}
	
	Game->IsValid = false;
	Game->UpdateAndRender = GameUpdateAndRenderStub;
	Game->GetSoundSamples = GameGetSoundSamplesStub;
}

internal void Win32LoadXInput() {
	HMODULE XInputLibrary = LoadLibraryA("xinput1_4.dll");	
	if (!XInputLibrary) { 
		XInputLibrary = LoadLibraryA("xinput9_1_0.dll");
	}

	if (!XInputLibrary) { 
		XInputLibrary = LoadLibraryA("xinput1_4.dll");
	}

	if (XInputLibrary) {
		XInputGetState = (x_input_get_state*)GetProcAddress(XInputLibrary, "XInputGetState");
		XInputSetState = (x_input_set_state*)GetProcAddress(XInputLibrary, "XInputSetState");
	} else {

	}
}

internal void Win32InitDSound(HWND Window, int32 SamplesPerSecond, int32 BufferSize) {
	// note(jax): Load the library
	HMODULE DSoundLibrary = LoadLibraryA("dsound.dll");

	if (DSoundLibrary) {
		// note(jax): Get a DirectSound object
		direct_sound_create* DirectSoundCreate = (direct_sound_create*)GetProcAddress(DSoundLibrary, "DirectSoundCreate");

		LPDIRECTSOUND DirectSound;
		if (DirectSoundCreate && SUCCEEDED(DirectSoundCreate(0, &DirectSound, 0))) {
			WAVEFORMATEX WaveFormat = { };
			WaveFormat.wFormatTag = WAVE_FORMAT_PCM;
			WaveFormat.nChannels = 2;
			WaveFormat.wBitsPerSample = 16;
			WaveFormat.nBlockAlign = (WaveFormat.nChannels * WaveFormat.wBitsPerSample) / 8;
			WaveFormat.nSamplesPerSec = SamplesPerSecond;
			WaveFormat.nAvgBytesPerSec = WaveFormat.nSamplesPerSec * WaveFormat.nBlockAlign;
			WaveFormat.cbSize = 0;

			if (SUCCEEDED(DirectSound->SetCooperativeLevel(Window, DSSCL_PRIORITY))) {
				DSBUFFERDESC BufferDescription = {};
				BufferDescription.dwSize = sizeof(DSBUFFERDESC);
				BufferDescription.dwFlags = DSBCAPS_PRIMARYBUFFER;

				LPDIRECTSOUNDBUFFER PrimaryBuffer;
				if (SUCCEEDED(DirectSound->CreateSoundBuffer(&BufferDescription, &PrimaryBuffer, 0))) {
					HRESULT Error = PrimaryBuffer->SetFormat(&WaveFormat);
					if (SUCCEEDED(Error)) {
						// note(jax): finally set the format!
						OutputDebugStringA("Primary Buffer was set.\n");
					} else {

					}
				} else {

				}

				BufferDescription = {};
				BufferDescription.dwSize = sizeof(DSBUFFERDESC);
				BufferDescription.dwFlags = DSBCAPS_GETCURRENTPOSITION2;
				BufferDescription.dwBufferBytes = BufferSize;
				BufferDescription.lpwfxFormat = &WaveFormat;

				HRESULT Error = DirectSound->CreateSoundBuffer(&BufferDescription, &GlobalSecondaryBuffer, 0);
				if (SUCCEEDED(Error)) {
					OutputDebugStringA("Secondary Buffer was created.\n");
				}
				else {

				}
			} else {

			}

		}
	}

}

internal win32_window_dimension GetWindowDimension(HWND Window) {
	win32_window_dimension Result;
	RECT ClientRect;
	GetClientRect(Window, &ClientRect);

	Result.Width = ClientRect.right - ClientRect.left;
	Result.Height = ClientRect.bottom - ClientRect.top;

	return Result;
}

// Resize or initalize a Device Independent Buffer
internal void Win32ResizeDIBSection(win32_offscreen_buffer* Buffer, int Width, int Height) {
	// todo(jax): Bulletproof this.
	// Maybe don't free first, free after, then free first if that fails.

	if (Buffer->Memory) {
		VirtualFree(Buffer->Memory, 0, MEM_RELEASE);
	}

	Buffer->Width = Width;
	Buffer->Height = Height;
	Buffer->BytesPerPixel = 4;

	Buffer->Info.bmiHeader.biSize = sizeof(Buffer->Info.bmiHeader);
	Buffer->Info.bmiHeader.biWidth = Buffer->Width;
	Buffer->Info.bmiHeader.biHeight = -Buffer->Height;
	Buffer->Info.bmiHeader.biPlanes = 1;
	Buffer->Info.bmiHeader.biBitCount = 32;
	Buffer->Info.bmiHeader.biCompression = BI_RGB;

	int BitmapMemorySize = (Buffer->Width * Buffer->Height) * Buffer->BytesPerPixel;
	Buffer->Memory = VirtualAlloc(0, BitmapMemorySize, MEM_COMMIT, PAGE_READWRITE);

	Buffer->Pitch = Buffer->Width * Buffer->BytesPerPixel;

	// tood(jax): Probably want to clear this to black.
}

internal void Win32DisplayBufferInWindow(win32_offscreen_buffer* Buffer, HDC DeviceContext, int WindowWidth, int WindowHeight) {
	StretchDIBits(DeviceContext, 0, 0, WindowWidth, WindowHeight, 0, 0, Buffer->Width, Buffer->Height, Buffer->Memory, &Buffer->Info, DIB_RGB_COLORS, SRCCOPY);
}

LRESULT CALLBACK MainWindowCallback(HWND Window, UINT Message, WPARAM wParam, LPARAM lParam) {
	LRESULT Result = 0;

	switch (Message) {
	case WM_ACTIVATEAPP: {
		if (wParam == TRUE) {
			SetLayeredWindowAttributes(Window, RGB(0, 0, 0), 255, LWA_ALPHA);
		} else {
			SetLayeredWindowAttributes(Window, RGB(0, 0, 0), 64, LWA_ALPHA);
		}
	} break;

	case WM_DESTROY: {
		// todo(jax): Handle this as an error -- recreate window?? how about swapping renderers mid game?
		GlobalRunning = false;
	} break;

	case WM_CLOSE: {
		// todo(jax): Handle this with a message to user
		GlobalRunning = false;
	} break;

#if 0
	case WM_SYSKEYDOWN:
	case WM_SYSKEYUP:
	case WM_KEYDOWN:
	case WM_KEYUP: {
		Assert(!"Keyboard input came in through a non-dispatch message!");
	} break;
#endif

	// todo(jax): RAW INPUT
	case WM_INPUT: {

	} break;

	case WM_PAINT: {
		PAINTSTRUCT Paint;
		HDC DeviceContext = BeginPaint(Window, &Paint);

		int X = Paint.rcPaint.left;
		int Y = Paint.rcPaint.top;
		int Width = Paint.rcPaint.right - Paint.rcPaint.left;
		int Height = Paint.rcPaint.bottom - Paint.rcPaint.top;

		win32_window_dimension Dimension = GetWindowDimension(Window);
		Win32DisplayBufferInWindow(&GlobalBackbuffer, DeviceContext, Dimension.Width, Dimension.Height);

		EndPaint(Window, &Paint);
	} break;

	default: {
		Result = DefWindowProcA(Window, Message, wParam, lParam);
	} break;
	}

	return Result;
}

internal void Win32ClearSoundBuffer(win32_sound_output* SoundOutput) {
	VOID* Region1;
	DWORD Region1Size;
	VOID* Region2;
	DWORD Region2Size;
	if (SUCCEEDED(GlobalSecondaryBuffer->Lock(0, SoundOutput->SecondaryBufferSize, &Region1, &Region1Size, &Region2, &Region2Size, 0))) { 
		int8* DestSample = (int8*)Region1;
		for (DWORD ByteIndex = 0; ByteIndex < Region1Size; ++ByteIndex) {
			*DestSample++ = 0;
		}

		DestSample = (int8*)Region2;
		for (DWORD ByteIndex = 0; ByteIndex < Region2Size; ++ByteIndex) {
			*DestSample++ = 0;
		}

		GlobalSecondaryBuffer->Unlock(Region1, Region1Size, Region2, Region2Size);
	}
}

internal void Win32FillSoundBuffer(win32_sound_output* SoundOutput, DWORD ByteToLock, DWORD BytesToWrite, game_sound_output_buffer* SourceBuffer) {
	VOID* Region1;
	DWORD Region1Size;
	VOID* Region2;
	DWORD Region2Size;

	if (SUCCEEDED(GlobalSecondaryBuffer->Lock(ByteToLock, BytesToWrite, &Region1, &Region1Size, &Region2, &Region2Size, 0))) {
		DWORD Region1SampleCount = Region1Size / SoundOutput->BytesPerSample;
		int16* DestSample = (int16*)Region1;
		int16* SourceSample = SourceBuffer->SampleOut;
		for (DWORD SampleIndex = 0; SampleIndex < Region1SampleCount; ++SampleIndex) {
			*DestSample++ = *SourceSample++;
			*DestSample++ = *SourceSample++;
			++SoundOutput->RunningSampleIndex;
		}

		DWORD Region2SampleCount = Region2Size / SoundOutput->BytesPerSample;
		DestSample = (int16*)Region2;
		for (DWORD SampleIndex = 0; SampleIndex < Region2SampleCount; ++SampleIndex) {
			*DestSample++ = *SourceSample++;
			*DestSample++ = *SourceSample++;
			++SoundOutput->RunningSampleIndex;
		}

		GlobalSecondaryBuffer->Unlock(Region1, Region1Size, Region2, Region2Size);
	}
}

internal void Win32ProcessKeyboardMessage(game_button_state* NewState, bool32 IsDown) {
	Assert(NewState->EndedDown != IsDown);
	NewState->EndedDown = IsDown;
	++NewState->HalfTransitionCount;
}

internal void Win32ProcessXInputDigitalButton(DWORD XInputButtonState, game_button_state* OldState, DWORD ButtonBit, game_button_state* NewState) {
	NewState->EndedDown = ((XInputButtonState & ButtonBit) == ButtonBit);
	NewState->HalfTransitionCount = (OldState->EndedDown == !NewState->EndedDown) ? 1 : 0;
}

internal void Win32BeginRecordingInput(win32_state* Win32State, int InputRecordingIndex) {
	Win32State->InputRecordingIndex = InputRecordingIndex;

	char* Filename = "foo.hmi";
	Win32State->RecordingHandle = CreateFileA(Filename, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
	DWORD BytesToWrite = (DWORD)Win32State->TotalSize;
	Assert(Win32State->TotalSize == BytesToWrite);
	DWORD BytesWritten;
	WriteFile(Win32State->RecordingHandle, Win32State->GameMemoryBlock, BytesToWrite, &BytesWritten, 0);
}

internal void Win23EndRecordingInput(win32_state* Win32State) {
	CloseHandle(Win32State->RecordingHandle);
	Win32State->InputRecordingIndex = 0;
}

internal void Win32BeginInputPlayback(win32_state* Win32State, int InputPlayingIndex) {
	Win32State->InputPlayingIndex = InputPlayingIndex;

	char* Filename = "foo.hmi";
	Win32State->PlaybackHandle = CreateFileA(Filename, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);	
	DWORD BytesToRead = (DWORD)Win32State->TotalSize;
	Assert(Win32State->TotalSize == BytesToRead);
	DWORD BytesRead;
	ReadFile(Win32State->PlaybackHandle, Win32State->GameMemoryBlock, BytesToRead, &BytesRead, 0);
}

internal void Win32EndInputPlayback(win32_state* Win32State) {
	CloseHandle(Win32State->PlaybackHandle);
	Win32State->InputPlayingIndex = 0;
}

internal void Win32RecordInput(win32_state* Win32State, game_input* NewInput) {
	DWORD BytesWritten;
	WriteFile(Win32State->RecordingHandle, NewInput, sizeof(*NewInput), &BytesWritten, 0);
}

internal void Win32PlayBackInput(win32_state* Win32State, game_input* NewInput) {
	DWORD BytesRead;
	if (ReadFile(Win32State->PlaybackHandle, NewInput, sizeof(*NewInput), &BytesRead, 0)) {
		// note(jax): There's still input
		if (BytesRead == 0) {
			// note(jax): We've hit the end of the stream, go back to the beginning
			int PlayingIndex = Win32State->InputPlayingIndex;
			Win32EndInputPlayback(Win32State);
			Win32BeginInputPlayback(Win32State, PlayingIndex);
		}
	} 
}

internal real32 Win32ProcessXInputStickValue(SHORT Value, SHORT DeadZoneThreshold) {
	real32 Result = 0;
	if (Value < -DeadZoneThreshold) {
		Result = (real32)Value / 32768.0f;
	} else if (Value > DeadZoneThreshold) {
		Result = (real32)Value / 32767.0f;
	}

	return Result;
}

internal void Win32ProcessPendingMessages(win32_state* Win32State, game_controller_input* KeyboardController) {
	MSG Message;
	while (PeekMessage(&Message, 0, 0, 0, PM_REMOVE)) {
		switch (Message.message) {
			case WM_QUIT: {
				GlobalRunning = false;
			} break;

			case WM_SYSKEYDOWN:
			case WM_SYSKEYUP:
			case WM_KEYDOWN:
			case WM_KEYUP: {
				uint32 VKCode = (uint32)Message.wParam;
				bool WasDown = ((Message.lParam & (1 << 30)) != 0);
				bool IsDown = ((Message.lParam & (1 << 31)) == 0);

				if (WasDown != IsDown) {
					if (VKCode == 'W') {
						Win32ProcessKeyboardMessage(&KeyboardController->MoveUp, IsDown);
					}
					else if (VKCode == 'A') {
						Win32ProcessKeyboardMessage(&KeyboardController->MoveLeft, IsDown);
					}
					else if (VKCode == 'S') {
						Win32ProcessKeyboardMessage(&KeyboardController->MoveDown, IsDown);
					}
					else if (VKCode == 'D') {
						Win32ProcessKeyboardMessage(&KeyboardController->MoveRight, IsDown);
					}
					else if (VKCode == 'Q') {
						Win32ProcessKeyboardMessage(&KeyboardController->LeftShoulder, IsDown);
					}
					else if (VKCode == 'E') {
						Win32ProcessKeyboardMessage(&KeyboardController->RightShoulder, IsDown);
					}
					else if (VKCode == VK_UP) {
						Win32ProcessKeyboardMessage(&KeyboardController->ActionUp, IsDown);
					}
					else if (VKCode == VK_DOWN) {
						Win32ProcessKeyboardMessage(&KeyboardController->ActionDown, IsDown);
					}
					else if (VKCode == VK_LEFT) {
						Win32ProcessKeyboardMessage(&KeyboardController->ActionLeft, IsDown);
					}
					else if (VKCode == VK_RIGHT) {
						Win32ProcessKeyboardMessage(&KeyboardController->ActionRight, IsDown);
					}
					else if (VKCode == VK_ESCAPE) {
						Win32ProcessKeyboardMessage(&KeyboardController->Back, IsDown);
					}
					else if (VKCode == VK_SPACE) {
						Win32ProcessKeyboardMessage(&KeyboardController->Start, IsDown);
					}

#if HANDMADE_INTERNAL
					else if (VKCode == 'P') {
						if (IsDown) {
							GlobalPause = !GlobalPause; 
						}
					} else if (VKCode == 'L') {
						if (IsDown) {
							if (Win32State->InputRecordingIndex == 0) {
								Win32BeginRecordingInput(Win32State, 1);
							}
							else {
								Win23EndRecordingInput(Win32State);
								Win32BeginInputPlayback(Win32State, 1);
							}
						}
					}
#endif
				}

				bool32 AltKeyWasDown = (Message.lParam & (1 << 29));
				if ((VKCode == VK_F4) && AltKeyWasDown) {
					GlobalRunning = false;
				}
			} break;

			default: {
				TranslateMessage(&Message);
				DispatchMessageA(&Message);
			} break;
		}
	}
}

inline LARGE_INTEGER Win32GetWallClock() {
	LARGE_INTEGER Result;
	QueryPerformanceCounter(&Result);
	return Result;
}

inline real32 Win32GetSecondsElapsed(LARGE_INTEGER Start, LARGE_INTEGER End) {
	real32 Result = ((real32)(End.QuadPart - Start.QuadPart) / (real32)GlobalPerfCountFrequency);
	return Result;
}

internal void Win32DEBUGDrawVertical(win32_offscreen_buffer* Backbuffer, int X, int Top, int Bottom, uint32 Color) {
	if (Top <= 0) {
		Top = 0;
	}

	if (Bottom > Backbuffer->Height) {
		Bottom = Backbuffer->Height;
	}

	if ((X >= 0) && (X < Backbuffer->Width)) {
		uint8* Pixel = ((uint8*)Backbuffer->Memory + X*Backbuffer->BytesPerPixel + Top*Backbuffer->Pitch);
		for (int Y = Top; Y < Bottom; ++Y) {
			*(uint32*)Pixel = Color;
			Pixel += Backbuffer->Pitch;
		}
	}
}

inline void Win32DrawSoundBufferMarker(win32_offscreen_buffer* Backbuffer, win32_sound_output* SoundOutput, real32 C, int PadX, int Top, int Bottom, DWORD Value, uint32 Color) {
	real32 XReal32 = (C * (real32)Value);
	int X = PadX + (int)XReal32;
	Win32DEBUGDrawVertical(Backbuffer, X, Top, Bottom, Color);
}

internal void Win32DEBUGSyncDisplay(win32_offscreen_buffer* Backbuffer, int MarkerCount, win32_debug_time_marker* Markers, int CurrentMarkerIndex, win32_sound_output* SoundOutput, real32 TargetSecondsPerFrame) {
	int PadX = 16;
	int PadY = 16;

	int LineHeight = 64;

	real32 C = (real32)(Backbuffer->Width - 2*PadX) / (real32)SoundOutput->SecondaryBufferSize;
	for (int MarkerIndex = 0; MarkerIndex < MarkerCount; ++MarkerIndex) {
		win32_debug_time_marker* ThisMarker = &Markers[MarkerIndex];
		Assert(ThisMarker->OutputPlayCursor < SoundOutput->SecondaryBufferSize);
		Assert(ThisMarker->OutputWriteCursor < SoundOutput->SecondaryBufferSize);
		Assert(ThisMarker->OutputLocation < SoundOutput->SecondaryBufferSize);
		Assert(ThisMarker->OutputByteCount < SoundOutput->SecondaryBufferSize);
		Assert(ThisMarker->FlipPlayCursor < SoundOutput->SecondaryBufferSize);
		Assert(ThisMarker->FlipWriteCursor < SoundOutput->SecondaryBufferSize);

		DWORD PlayColor = 0xFFFFFFFF;
		DWORD WriteColor = 0xFFFF0000;
		DWORD ExpectedFlipColor = 0xFFFFFF00;
		DWORD PlayWindowColor = 0xFFFF00FF;

		int Top = PadY;
		int Bottom = PadY + LineHeight;
		if (MarkerIndex == CurrentMarkerIndex) {
			Top += LineHeight + PadY;
			Bottom += LineHeight + PadY;

			int FirstTop = Top;
			Win32DrawSoundBufferMarker(Backbuffer, SoundOutput, C, PadX, Top, Bottom, ThisMarker->OutputPlayCursor, PlayColor);
			Win32DrawSoundBufferMarker(Backbuffer, SoundOutput, C, PadX, Top, Bottom, ThisMarker->OutputWriteCursor, WriteColor);

			Top += LineHeight + PadY;
			Bottom += LineHeight + PadY;

			Win32DrawSoundBufferMarker(Backbuffer, SoundOutput, C, PadX, Top, Bottom, ThisMarker->OutputLocation, PlayColor);
			Win32DrawSoundBufferMarker(Backbuffer, SoundOutput, C, PadX, Top, Bottom, ThisMarker->OutputLocation + ThisMarker->OutputByteCount, WriteColor);
			
			Top += LineHeight + PadY;
			Bottom += LineHeight + PadY;

			Win32DrawSoundBufferMarker(Backbuffer, SoundOutput, C, PadX, FirstTop, Bottom, ThisMarker->ExpectedFlipPlayCursor, ExpectedFlipColor);
		}

		Win32DrawSoundBufferMarker(Backbuffer, SoundOutput, C, PadX, Top, Bottom, ThisMarker->FlipPlayCursor, PlayColor);
		Win32DrawSoundBufferMarker(Backbuffer, SoundOutput, C, PadX, Top, Bottom, ThisMarker->FlipPlayCursor + 480*SoundOutput->BytesPerSample, PlayWindowColor);
		Win32DrawSoundBufferMarker(Backbuffer, SoundOutput, C, PadX, Top, Bottom, ThisMarker->FlipWriteCursor, WriteColor);
	}
}

internal void CatStrings(size_t SourceACount, char* SourceA, size_t SourceBCount, char* SourceB, size_t DestCount, char* Dest) {
	// todo(jax): Dest bounds checking!
	for (int Index = 0; Index < SourceACount; ++Index) {
		*Dest++ = *SourceA++;
	}

	for (int Index = 0; Index < SourceBCount; ++Index) {
		*Dest++ = *SourceB++;
	}

	*Dest++ = 0;
}

int CALLBACK WinMain(HINSTANCE Instance, HINSTANCE PrevInstance, LPSTR CommandLine, int nCmdShow) {
	// note(jax): Never use MAX_PATH in code that is user-facing, because it
	// can be dangerous and lead to bad results.
	char EXEFileName[MAX_PATH];
	DWORD SizeOfFilename = GetModuleFileNameA(0, EXEFileName, sizeof(EXEFileName));
	char* OnePastLastSlash = EXEFileName;
	for (char* Scan = EXEFileName; *Scan; ++Scan) {
		if (*Scan == '\\') {
			OnePastLastSlash = Scan + 1;
		}
	}

	char SourceGameCodeDLLFilename[] = "handmade.dll";
	char SourceGameCodeDLLFullPath[MAX_PATH];
	CatStrings(OnePastLastSlash - EXEFileName, EXEFileName, sizeof(SourceGameCodeDLLFilename) - 1, SourceGameCodeDLLFilename, sizeof(SourceGameCodeDLLFullPath), SourceGameCodeDLLFullPath);

	char TempGameCodeDLLFilename[] = "handmade_temp.dll";
	char TempGameCodeDLLFullPath[MAX_PATH];
	CatStrings(OnePastLastSlash - EXEFileName, EXEFileName, sizeof(TempGameCodeDLLFilename) - 1, TempGameCodeDLLFilename, sizeof(TempGameCodeDLLFullPath), TempGameCodeDLLFullPath);

	LARGE_INTEGER PerfFrequencyResult;
	QueryPerformanceFrequency(&PerfFrequencyResult);
	GlobalPerfCountFrequency = PerfFrequencyResult.QuadPart;

	// note(jax): Set the Windows scheduler granularity to 1
	UINT DesiredSchedulerMS = 1;
	bool32 SleepIsGranular = (timeBeginPeriod(DesiredSchedulerMS) == TIMERR_NOERROR);

	Win32LoadXInput();

	WNDCLASSA WindowClass = {};

	Win32ResizeDIBSection(&GlobalBackbuffer, 1280, 720);

	WindowClass.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
	WindowClass.lpfnWndProc = MainWindowCallback;
	WindowClass.hInstance = Instance;
	WindowClass.lpszClassName = "HandmadeHeroWindowClass";

	// todo(jax): How do we reliably query this on Windows?
#define MonitorRefreshHz 60
#define GameUpdateHz (MonitorRefreshHz / 2)
	real32 TargetSecondsPerFrame = 1.f / (real32)GameUpdateHz;

	if (RegisterClassA(&WindowClass)) {
		HWND Window = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED, 
			WindowClass.lpszClassName, 
			"Handmade Hero x64", 
			WS_OVERLAPPEDWINDOW | WS_VISIBLE, 
			0, 
			0, 
			1280, 
			720, 
			0, 
			0, 
			Instance, 
			0);

		if (Window) {
			win32_sound_output SoundOutput = {};

			SoundOutput.SamplesPerSecond = 40000;
			SoundOutput.BytesPerSample = sizeof(int16) * 2;
			SoundOutput.SecondaryBufferSize = SoundOutput.SamplesPerSecond * SoundOutput.BytesPerSample;
			// todo(jax): Get rid of LatencySampleCount
			SoundOutput.LatencySampleCount = 3*(SoundOutput.SamplesPerSecond / GameUpdateHz);
			SoundOutput.SafetyBytes = (SoundOutput.SamplesPerSecond * SoundOutput.BytesPerSample / GameUpdateHz) / 2;
			Win32InitDSound(Window, SoundOutput.SamplesPerSecond, SoundOutput.SecondaryBufferSize);
			Win32ClearSoundBuffer(&SoundOutput);
			GlobalSecondaryBuffer->Play(0, 0, DSBPLAY_LOOPING);

			win32_state Win32State = {};
			GlobalRunning = true;

#if 0
			// note(jax): This tests the PlayCursor/WriteCursor update frequency
			while (GlobalRunning) {					
				DWORD PlayCursor;
				DWORD WriteCursor;
				GlobalSecondaryBuffer->GetCurrentPosition(&PlayCursor, &WriteCursor);

				char TextBuffer[256];
				_snprintf_s(TextBuffer, sizeof(TextBuffer), "PC:%u  WC:%u\n", PlayCursor, WriteCursor);
				OutputDebugStringA(TextBuffer);
			}
#endif

			int16* Samples = (int16*)VirtualAlloc(0, SoundOutput.SecondaryBufferSize, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);

#if HANDMADE_INTERNAL
			LPVOID BaseAddress = (LPVOID)Terabytes(2);
#else
			LPVOID BaseAddress = 0;
#endif
			
			game_memory GameMemory = {};
			GameMemory.PermanentStorageSize = Megabytes(64);
			GameMemory.TransientStorageSize = Gigabytes(1);
			GameMemory.DEBUGPlatformFreeFileMemory = DEBUGPlatformFreeFileMemory;
			GameMemory.DEBUGPlatformReadEntireFile = DEBUGPlatformReadEntireFile;
			GameMemory.DEBUGPlatformWriteEntireFile = DEBUGPlatformWriteEntireFile;

			// todo(jax): Handle various memory footprints (USING SYSTEM_METRICS)
			Win32State.TotalSize = GameMemory.PermanentStorageSize + GameMemory.TransientStorageSize;
			Win32State.GameMemoryBlock = VirtualAlloc(BaseAddress, (size_t)Win32State.TotalSize, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
			GameMemory.PermanentStorage = Win32State.GameMemoryBlock;
			GameMemory.TransientStorage = ((uint8*)GameMemory.PermanentStorage + GameMemory.PermanentStorageSize);

			if (Samples && GameMemory.PermanentStorage && GameMemory.TransientStorage) {
				game_input Input[2] = {};
				game_input* NewInput = &Input[0];
				game_input* OldInput = &Input[1];

				LARGE_INTEGER LastCounter = Win32GetWallClock();
				LARGE_INTEGER FlipWallClock = Win32GetWallClock();

				int DEBUGTimeMarkerIndex = 0;
				win32_debug_time_marker DEBUGTimeMarkers[GameUpdateHz / 2] = {0};

				DWORD AudioLatencyBytes = 0;
				real32 AudioLatencySeconds = 0;
				bool32 SoundIsValid = false;

				char* SourceDLLName = "handmade.dll";
				win32_game_code Game = Win32LoadGameCode(SourceGameCodeDLLFullPath, TempGameCodeDLLFullPath);

				int64 LastCycleCount = __rdtsc();
				while (GlobalRunning) {
					FILETIME NewDLLWriteTime = Win32GetLastWriteTime(SourceDLLName);
					if (CompareFileTime(&NewDLLWriteTime, &Game.DLLLastWriteTime) != 0) {
						Win32UnloadGameCode(&Game);
						Game = Win32LoadGameCode(SourceGameCodeDLLFullPath, TempGameCodeDLLFullPath);
					}

					Assert(Game.IsValid);

					// todo(jax): Zeroing macro
					// todo(jax): We can't zero everything because the up/down state will
					// be wrong!!!
					game_controller_input* OldKeyboardController = GetController(OldInput, 0);
					game_controller_input* NewKeyboardController = GetController(NewInput, 0);
					game_controller_input ZeroController = {};
					*NewKeyboardController = ZeroController;
					NewKeyboardController->IsConnected = true;
					for (int ButtonIndex = 0; ButtonIndex < ArrayCount(NewKeyboardController->Buttons); ++ButtonIndex) {
						NewKeyboardController->Buttons[ButtonIndex].EndedDown = OldKeyboardController->Buttons[ButtonIndex].EndedDown;
					}

					Win32ProcessPendingMessages(&Win32State, NewKeyboardController);

					if (!GlobalPause) {
						// todo(jax): Need to avoid polling disconnected controllers to avoid
						// xinput frame rate hit on older library revisions.
						// todo(jax): Should we poll this more frequently
						DWORD MaxControllerCount = XUSER_MAX_COUNT;
						if (MaxControllerCount > (ArrayCount(NewInput->Controllers) - 1)) {
							MaxControllerCount = (ArrayCount(NewInput->Controllers) - 1);
						}

						for (DWORD ControllerIndex = 0; ControllerIndex < MaxControllerCount; ++ControllerIndex) {
							DWORD OurControllerIndex = ControllerIndex + 1;
							game_controller_input* OldController = GetController(OldInput, OurControllerIndex);
							game_controller_input* NewController = GetController(NewInput, OurControllerIndex);

							XINPUT_STATE ControllerState;
							if (XInputGetState(ControllerIndex, &ControllerState) == ERROR_SUCCESS) {
								NewController->IsConnected = true;
								// note(jaxon): Controller is plugged in
								// todo(jaxon): See if ControllerState.dwPacketNumber increments too rapidly
								XINPUT_GAMEPAD* Pad = &ControllerState.Gamepad;

								// note(jax): Consider applying different linear response curve transformations on
								// the deadzone, like Modern Warfare does.
								// https://docs.microsoft.com/en-us/windows/win32/xinput/getting-started-with-xinput#dead-zone
								NewController->IsAnalog = true;
								NewController->StickAverageX = Win32ProcessXInputStickValue(Pad->sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
								NewController->StickAverageY = Win32ProcessXInputStickValue(Pad->sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);

								if (Pad->wButtons & XINPUT_GAMEPAD_DPAD_UP) {
									NewController->StickAverageY = 1.0f;
								}

								if (Pad->wButtons & XINPUT_GAMEPAD_DPAD_DOWN) {
									NewController->StickAverageY = -1.0f;
								}

								if (Pad->wButtons & XINPUT_GAMEPAD_DPAD_LEFT) {
									NewController->StickAverageX = -1.0f;
								}

								if (Pad->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) {
									NewController->StickAverageX = 1.0f;
								}

								// todo(jax): Min/max macros!!!

								real32 Threshold = 0.5f;
								Win32ProcessXInputDigitalButton((NewController->StickAverageX < -Threshold) ? 1 : 0, &OldController->MoveLeft, 1, &NewController->MoveLeft);
								Win32ProcessXInputDigitalButton((NewController->StickAverageX < -Threshold) ? 1 : 0, &OldController->MoveLeft, 1, &NewController->MoveLeft);					
								Win32ProcessXInputDigitalButton((NewController->StickAverageY < -Threshold) ? 1 : 0, &OldController->MoveLeft, 1, &NewController->MoveLeft);
								Win32ProcessXInputDigitalButton((NewController->StickAverageY < -Threshold) ? 1 : 0, &OldController->MoveLeft, 1, &NewController->MoveLeft);

								Win32ProcessXInputDigitalButton(Pad->wButtons, &OldController->ActionUp, XINPUT_GAMEPAD_Y, &NewController->ActionUp);						
								Win32ProcessXInputDigitalButton(Pad->wButtons, &OldController->ActionDown, XINPUT_GAMEPAD_A, &NewController->ActionDown);
								Win32ProcessXInputDigitalButton(Pad->wButtons, &OldController->ActionLeft, XINPUT_GAMEPAD_X, &NewController->ActionLeft);
								Win32ProcessXInputDigitalButton(Pad->wButtons, &OldController->ActionRight, XINPUT_GAMEPAD_B, &NewController->ActionRight);
								Win32ProcessXInputDigitalButton(Pad->wButtons, &OldController->LeftShoulder, XINPUT_GAMEPAD_LEFT_SHOULDER, &NewController->LeftShoulder);
								Win32ProcessXInputDigitalButton(Pad->wButtons, &OldController->RightShoulder, XINPUT_GAMEPAD_RIGHT_SHOULDER, &NewController->RightShoulder);

								Win32ProcessXInputDigitalButton(Pad->wButtons, &OldController->Start, XINPUT_GAMEPAD_START, &NewController->Start);
								Win32ProcessXInputDigitalButton(Pad->wButtons, &OldController->Back, XINPUT_GAMEPAD_BACK, &NewController->Back);
							} else {
								NewController->IsConnected = false;
							}
						}
						
						game_offscreen_buffer Buffer = {};
						Buffer.Memory = GlobalBackbuffer.Memory;
						Buffer.Width = GlobalBackbuffer.Width;
						Buffer.Height = GlobalBackbuffer.Height;
						Buffer.Pitch = GlobalBackbuffer.Pitch;
						Buffer.BytesPerPixel = GlobalBackbuffer.BytesPerPixel;

						if (Win32State.InputRecordingIndex != 0) {
							Win32RecordInput(&Win32State, NewInput);
						}

						if (Win32State.InputPlayingIndex != 0) {
							Win32PlayBackInput(&Win32State, NewInput);
						}

						Game.UpdateAndRender(&GameMemory, NewInput, &Buffer);

						LARGE_INTEGER AudioWallClock = Win32GetWallClock();
						real32 FromBeginToAudioSeconds = Win32GetSecondsElapsed(FlipWallClock, AudioWallClock);

						DWORD PlayCursor;
						DWORD WriteCursor;
						if (GlobalSecondaryBuffer->GetCurrentPosition(&PlayCursor, &WriteCursor) == DS_OK) {
							/* note(jax):

								Here is how sound output computation works.

								We define a safety value that is the number
								of samples we think our game update look
								may vary by (let's say up to 2ms)

								When we wake up to write audio, we will look 
								and see what the play cursor position is and we 
								will forecast ahead where we think the play 
								cursor will be on the next frame boundary.

								We will then look to see if the write cursor is
								before that by at least our safety value. If 
								it is, the target fill position is that frame 
								boundary plus one frame. This gives us perfect 
								audio  sync in the case of a card that has low 
								enough latency.

								If the write cursor is _after_ that safety
								margin, then we assume we can never sync the 
								audio perfectly, so we will write one frame's
								worth of audio plus the safety margin's worth
								of guard samples.
							*/
	#if HANDMADE_INTERNAL
							win32_debug_time_marker* Marker = &DEBUGTimeMarkers[DEBUGTimeMarkerIndex];
	#endif
							if (!SoundIsValid) {
								SoundOutput.RunningSampleIndex = WriteCursor / SoundOutput.BytesPerSample;
								SoundIsValid = true;
							}

							DWORD ByteToLock = ((SoundOutput.RunningSampleIndex * SoundOutput.BytesPerSample) % SoundOutput.SecondaryBufferSize);

							DWORD ExpectedSoundBytesPerFrame = (SoundOutput.SamplesPerSecond * SoundOutput.BytesPerSample) / GameUpdateHz;
							real32 SecondsLeftUntilFlip = (TargetSecondsPerFrame - FromBeginToAudioSeconds);
							DWORD ExpectedBytesUntilFlip = (DWORD)((SecondsLeftUntilFlip/TargetSecondsPerFrame)*(real32)ExpectedSoundBytesPerFrame);
							DWORD ExpectedFrameBoundaryByte = PlayCursor + ExpectedSoundBytesPerFrame;

							DWORD SafeWriteCursor = WriteCursor;
							if (SafeWriteCursor < PlayCursor) {
								SafeWriteCursor += SoundOutput.SecondaryBufferSize;
							}
							Assert(SafeWriteCursor >= PlayCursor);
							SafeWriteCursor += SoundOutput.SafetyBytes;
							bool32 AudioCardIsLowLatency = (SafeWriteCursor < ExpectedFrameBoundaryByte);

							DWORD TargetCursor = 0;
							if (AudioCardIsLowLatency) {
								TargetCursor = (ExpectedFrameBoundaryByte + ExpectedSoundBytesPerFrame);
							} else {
								TargetCursor = (WriteCursor + ExpectedSoundBytesPerFrame + SoundOutput.SafetyBytes);
							}
							TargetCursor = TargetCursor % SoundOutput.SecondaryBufferSize;

							DWORD BytesToWrite = 0;
							if (ByteToLock > TargetCursor) {
								BytesToWrite = (SoundOutput.SecondaryBufferSize - ByteToLock);
								BytesToWrite += TargetCursor;
							} else {
								BytesToWrite = TargetCursor - ByteToLock;
							}

							game_sound_output_buffer SoundBuffer = {};
							SoundBuffer.SamplesPerSecond = SoundOutput.SamplesPerSecond;
							SoundBuffer.SampleCount = BytesToWrite / SoundOutput.BytesPerSample;
							SoundBuffer.SampleOut = Samples;
							Game.GetSoundSamples(&GameMemory, &SoundBuffer); 

	#if HANDMADE_INTERNAL
							Marker->OutputPlayCursor = PlayCursor;
							Marker->OutputWriteCursor = WriteCursor;
							Marker->OutputLocation = ByteToLock;
							Marker->OutputByteCount = BytesToWrite;
							Marker->ExpectedFlipPlayCursor = ExpectedFrameBoundaryByte;

							DWORD UnwrappedWriteCursor = WriteCursor;
							if (UnwrappedWriteCursor < PlayCursor) {
								UnwrappedWriteCursor += SoundOutput.SecondaryBufferSize;
							}
							// https://guide.handmadehero.org/code/day020/#1651
							AudioLatencyBytes = UnwrappedWriteCursor - PlayCursor;
							AudioLatencySeconds = (((real32)AudioLatencyBytes / (real32)SoundOutput.BytesPerSample) / (real32)SoundOutput.SamplesPerSecond);

							char TextBuffer[256];
							_snprintf_s(TextBuffer, sizeof(TextBuffer), "BTL:%u TC:%u BTW:%u - PC:%u WC:%u DELTA:%u (%fs)\n", ByteToLock, TargetCursor, BytesToWrite, PlayCursor, WriteCursor, AudioLatencyBytes, AudioLatencySeconds);
							OutputDebugStringA(TextBuffer);
	#endif
							Win32FillSoundBuffer(&SoundOutput, ByteToLock, BytesToWrite, &SoundBuffer);
						} else {
							SoundIsValid = false;
						}

						LARGE_INTEGER WorkCounter = Win32GetWallClock();
						real32 WorkSecondsElapsed = Win32GetSecondsElapsed(LastCounter, WorkCounter);

						// todo(jax): NOT TESTED YET! PROBABLY BUGGY!!!
						real32 SecondsElapsedForFrame = WorkSecondsElapsed;
						if (SecondsElapsedForFrame < TargetSecondsPerFrame) {
							if (SleepIsGranular) {
								DWORD SleepMS = (DWORD)(1000.f * (TargetSecondsPerFrame - SecondsElapsedForFrame));
								if (SleepMS > 0) {
									Sleep(SleepMS);
								}
							}

							real32 TestSecondsElapsedForFrame = Win32GetSecondsElapsed(LastCounter, Win32GetWallClock());
	//						Assert(TestSecondsElapsedForFrame < TargetSecondsPerFrame);
							while (SecondsElapsedForFrame < TargetSecondsPerFrame) {
								SecondsElapsedForFrame = Win32GetSecondsElapsed(LastCounter, Win32GetWallClock());
							}
						} else {
							// todo(jax): MISSED FRAME RATE!
							// todo(jax): Logging
						}

						LARGE_INTEGER EndCounter = Win32GetWallClock();
						real32 MSPerFrame = 1000.f * Win32GetSecondsElapsed(LastCounter, EndCounter);
						LastCounter = EndCounter;

						win32_window_dimension Dimension = GetWindowDimension(Window);
	#if HANDMADE_INTERNAL
						// note(jax): Current is wrong on the zero'th index
						Win32DEBUGSyncDisplay(&GlobalBackbuffer, ArrayCount(DEBUGTimeMarkers), DEBUGTimeMarkers, DEBUGTimeMarkerIndex - 1, &SoundOutput, TargetSecondsPerFrame);
	#endif
						HDC DeviceContext = GetDC(Window);
						Win32DisplayBufferInWindow(&GlobalBackbuffer, DeviceContext, Dimension.Width, Dimension.Height);
						ReleaseDC(Window, DeviceContext);

						FlipWallClock = Win32GetWallClock();
	#if HANDMADE_INTERNAL
						// note(jax): This is debug code
						{
							DWORD _PlayCursor;
							DWORD _WriteCursor;
							if (GlobalSecondaryBuffer->GetCurrentPosition(&_PlayCursor, &_WriteCursor) == DS_OK) {
								Assert(DEBUGTimeMarkerIndex < ArrayCount(DEBUGTimeMarkers));
								win32_debug_time_marker* Marker = &DEBUGTimeMarkers[DEBUGTimeMarkerIndex];
								Marker->FlipPlayCursor = _PlayCursor;
								Marker->FlipWriteCursor = _WriteCursor;
							}
						}
	#endif

						game_input* Temp = NewInput;
						NewInput = OldInput;
						OldInput = Temp;
						// todo(jax): Should I clear these here?

						uint64 EndCycleCount = __rdtsc();
						uint64 CyclesElapsed = EndCycleCount - LastCycleCount;
						LastCycleCount = EndCycleCount;

						real64 FPS = 0.f;
						real64 MCPF = ((real64)(CyclesElapsed / (1000.f * 1000.f)));

						char FPSBuffer[256];
						_snprintf_s(FPSBuffer, sizeof(FPSBuffer), "%.02fms, %.02fFPS, %.02fmc/frame\n", MSPerFrame, FPS, MCPF); // MSPerFrame, FPS, MegacyclesPerFrame
						OutputDebugStringA(FPSBuffer);
	#if HANDMADE_INTERNAL
						++DEBUGTimeMarkerIndex;
						if (DEBUGTimeMarkerIndex >= ArrayCount(DEBUGTimeMarkers)) {
							DEBUGTimeMarkerIndex = 0;
						}
	#endif
					}
				}
			} else {
				// note(jax): GameMemory failed
			}
		} else {

		}
	}

    return 0;
}