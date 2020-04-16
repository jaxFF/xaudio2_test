#ifndef HANDMADE_H

/*
	note(jax):

	HANDMADE_INTERNAL:
		0 - Build for public release
		1 - Build for developer only

	HANDMADE_SLOW:
		0 - No slow code allowed
		1 - Slow code permitted
*/

// todo(jax): Implement sine ourselves
#include <math.h>
#include <stdint.h>

#define internal static
#define local_persist static
#define global_variable static

#define Pi32 3.14159265359f

typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;
typedef int32 bool32;

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

typedef float real32;
typedef double real64;

// note(jax): Platform-independent way to perform an assertion.
// Flat out writes to zero memory to crash the program.
#if HANDMADE_SLOW
#define Assert(Expression) if (!(Expression)) { *(int*)0=0; }
#else
#define Assert(Expression)
#endif

// todo(jax): Should these always be 64-bit?
#define Kilobytes(Value) (((uint64)Value) * 1024LL)
#define Megabytes(Value) (Kilobytes((uint64)Value) * 1024LL)
#define Gigabytes(Value) (Megabytes((uint64)Value) * 1024LL)
#define Terabytes(Value) (Gigabytes((uint64)Value) * 1024LL)

#define ArrayCount(Array) (sizeof(Array) / sizeof((Array)[0]))
// todo(jax): swap, min, max ... macros???

inline uint32 SafeTruncateUInt64(uint64 Value) {
	// todo(jax): Defines for min/max values UInt32Max
	Assert(Value <= 0xFFFFFFFF);
	uint32 Result = (uint32)Value;
	return Result;
}

// note(jax): Services that the platform layer provides to the game.
#if HANDMADE_INTERNAL
/* important(jax):

	These are NOT for doing anything in the shipped game - they are
	blocking and the write doesn't protect against lost data!
*/
struct debug_read_file_result {
	uint32 ContentsSize;
	void* Contents;
};

#define DEBUG_PLATFORM_FREE_FILE_MEMORY(name) void name(void* Memory)
typedef DEBUG_PLATFORM_FREE_FILE_MEMORY(debug_platform_free_file_memory);

#define DEBUG_PLATFORM_READ_ENTIRE_FILE(name) debug_read_file_result name(char* Filename)
typedef DEBUG_PLATFORM_READ_ENTIRE_FILE(debug_platform_read_entire_file);

#define DEBUG_PLATFORM_WRITE_ENTIRE_FILE(name) bool32 name(char* Filename, uint32 MemorySize, void* Memory)
typedef DEBUG_PLATFORM_WRITE_ENTIRE_FILE(debug_platform_write_entire_file);
#endif

// note(jax): Services that the game provides to the platform layer.
// (this may expand in the future -- sound on seperate thread, etc..)

// FOUR THINGS - timings, controllor/kb input, bitmap buffer to use, sound buffer to use

// todo(jax): In the future, rendering _specifically_ will become a three-tiered abstraction!!!
struct game_offscreen_buffer {
	void* Memory; // note(jax): Pixels are always 32-bits wide, so Memory Order BB GG RR XX
	int Width;
	int Height;
	int Pitch;
	int BytesPerPixel;
};

struct game_sound_output_buffer {
	int SamplesPerSecond;
	int SampleCount;
	int16* SampleOut;
};

struct game_button_state {
	int HalfTransitionCount;
	bool32 EndedDown;
};

struct game_controller_input {
	bool32 IsConnected;
	bool32 IsAnalog;
	real32 StickAverageX;
	real32 StickAverageY;

	union {
		game_button_state Buttons[12];
		struct {
			game_button_state MoveUp;
			game_button_state MoveDown;
			game_button_state MoveLeft;
			game_button_state MoveRight;

			game_button_state ActionUp;
			game_button_state ActionDown;
			game_button_state ActionLeft;
			game_button_state ActionRight;

			game_button_state LeftShoulder;
			game_button_state RightShoulder;

			game_button_state Start;
			game_button_state Back;
		};
	};
};

struct game_input {
	// todo(jax): Insert clock values here?
	game_controller_input Controllers[5];
};

inline game_controller_input* GetController(game_input* Input, int ControllerIndex) {
	Assert(ControllerIndex < ArrayCount(Input->Controllers));

	game_controller_input* Result = &Input->Controllers[ControllerIndex];
	return Result;
}

struct game_memory {
	bool32 IsInitalized;
	uint64 PermanentStorageSize;
	void* PermanentStorage; // note(jax): REQUIRED to be cleared to zero at startup!!!

	uint64 TransientStorageSize;
	void* TransientStorage; // note(jax): REQUIRED to be cleared to zero at startup!!!

	debug_platform_free_file_memory* DEBUGPlatformFreeFileMemory;
	debug_platform_read_entire_file* DEBUGPlatformReadEntireFile;
	debug_platform_write_entire_file* DEBUGPlatformWriteEntireFile;
};

#define GAME_UPDATE_AND_RENDER(name) void name(game_memory* Memory, game_input* Input, game_offscreen_buffer* Buffer)
typedef GAME_UPDATE_AND_RENDER(game_update_and_render);
GAME_UPDATE_AND_RENDER(GameUpdateAndRenderStub) {

}

// note(jax): At the moment, this needs to be a very fast function, it cannot be
// more than a millisecond or so.
// todo(jax): Reduce the pressure on the function's performance by measuring it
// or asking about it, etc.
#define GAME_GET_SOUND_SAMPLES(name) void name(game_memory* Memory, game_sound_output_buffer* SoundBuffer)
typedef GAME_GET_SOUND_SAMPLES(game_get_sound_samples);
GAME_GET_SOUND_SAMPLES(GameGetSoundSamplesStub) {

}

//
//
//

struct game_state {
	int ToneHz;
	int GreenOffset;
	int BlueOffset;
	real32 tSine;

	int PlayerX;
	int PlayerY;
	real32 tJump;
};

#define HANDMADE_H
#endif