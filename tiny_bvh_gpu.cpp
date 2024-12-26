// This example shows how to build a basic GPU path tracer using
// tinybvh. TinyOCL is used to render on the GPU using OpenCL.

#define FENSTER_APP_IMPLEMENTATION
#define SCRWIDTH 1280
#define SCRHEIGHT 720
#include "external/fenster.h" // https://github.com/zserge/fenster

// This application uses tinybvh - And this file will include the implementation.
#define TINYBVH_IMPLEMENTATION
#include "tiny_bvh.h"
using namespace tinybvh;

// This application uses tinyocl - And this file will include the implementation.
#define TINY_OCL_IMPLEMENTATION
#include "tiny_ocl.h"

// Other includes
#include <fstream>

// Application variables

static BVH8_CWBVH bvh;
static bvhvec4* tris = 0;
static int triCount = 0, frameIdx = 0, spp = 0;
static Kernel* init, * clear, * rayGen, * extend, * shade;
static Kernel* updateCounters1, * updateCounters2, * traceShadows, * finalize;
static Buffer* pixels, * accumulator, * raysIn, * raysOut, * connections, * triData;
static Buffer* cwbvhNodes = 0, * cwbvhTris = 0, * noise = 0;
static size_t computeUnits;
static uint32_t* blueNoise = new uint32_t[128 * 128 * 8];

// View pyramid for a pinhole camera
struct RenderData
{
	bvhvec4 eye = bvhvec4( 0, 30, 0, 0 ), view = bvhvec4( -1, 0, 0, 0 ), C, p0, p1, p2;
	uint32_t frameIdx, dummy1, dummy2, dummy3;
} rd;

// Scene management - Append a file, with optional position, scale and color override, tinyfied
void AddMesh( const char* file, float scale = 1, bvhvec3 pos = {}, int c = 0, int N = 0 )
{
	std::fstream s{ file, s.binary | s.in }; s.read( (char*)&N, 4 );
	bvhvec4* data = (bvhvec4*)tinybvh::malloc64( (N + triCount) * 48 );
	if (tris) memcpy( data, tris, triCount * 48 ), tinybvh::free64( tris );
	tris = data, s.read( (char*)tris + triCount * 48, N * 48 ), triCount += N;
	for (int* b = (int*)tris + (triCount - N) * 12, i = 0; i < N * 3; i++)
		*(bvhvec3*)b = *(bvhvec3*)b * scale + pos, b[3] = c ? c : b[3], b += 4;
}
void AddQuad( const bvhvec3 pos, const float w, const float d, int c )
{
	bvhvec4* data = (bvhvec4*)tinybvh::malloc64( (triCount + 2) * 48 );
	if (tris) memcpy( data + 6, tris, triCount * 48 ), tinybvh::free64( tris );
	data[0] = bvhvec3( -w, 0, -d ), data[1] = bvhvec3( w, 0, -d );
	data[2] = bvhvec3( w, 0, d ), data[3] = bvhvec3( -w, 0, -d ), tris = data;
	data[4] = bvhvec3( w, 0, d ), data[5] = bvhvec3( -w, 0, d ), triCount += 2;
	for (int i = 0; i < 6; i++) data[i] = 0.5f * data[i] + pos, data[i].w = *(float*)&c;
}

// Blue noise from file
void LoadBlueNoise()
{
	std::fstream s{ "./testdata/blue_noise_128x128x8_2d.raw", s.binary | s.in };
	s.read( (char*)blueNoise, 128 * 128 * 8 * 4 );
}

// Application init
void Init()
{
	// create OpenCL kernels
	init = new Kernel( "wavefront.cl", "SetRenderData" );
	clear = new Kernel( "wavefront.cl", "Clear" );
	rayGen = new Kernel( "wavefront.cl", "Generate" );
	extend = new Kernel( "wavefront.cl", "Extend" );
	shade = new Kernel( "wavefront.cl", "Shade" );
	updateCounters1 = new Kernel( "wavefront.cl", "UpdateCounters1" );
	updateCounters2 = new Kernel( "wavefront.cl", "UpdateCounters2" );
	traceShadows = new Kernel( "wavefront.cl", "Connect" );
	finalize = new Kernel( "wavefront.cl", "Finalize" );
	// we need the 'compute unit' or 'SM' count for wavefront rendering; ask OpenCL for it.
	clGetDeviceInfo( init->GetDeviceID(), CL_DEVICE_MAX_COMPUTE_UNITS, sizeof( size_t ), &computeUnits, NULL );
	// create OpenCL buffers for wavefront path tracing
	int N = SCRWIDTH * SCRHEIGHT;
	pixels = new Buffer( N * sizeof( uint32_t ) );
	accumulator = new Buffer( N * sizeof( bvhvec4 ) );
	raysIn = new Buffer( N * sizeof( bvhvec4 ) * 4 );
	raysOut = new Buffer( N * sizeof( bvhvec4 ) * 4 );
	connections = new Buffer( N * 3 * sizeof( bvhvec4 ) * 3 );
	accumulator = new Buffer( N * sizeof( bvhvec4 ) );
	pixels = new Buffer( N * sizeof( uint32_t ) );
	LoadBlueNoise();
	noise = new Buffer( 128 * 128 * 8 * sizeof( uint32_t ), blueNoise );
	noise->CopyToDevice();
	// load raw vertex data
	AddMesh( "./testdata/cryteksponza.bin", 1, bvhvec3( 0 ), 0xffffff );
	AddMesh( "./testdata/lucy.bin", 1.1f, bvhvec3( -2, 4.1f, -3 ), 0x2ffff88 );
	AddQuad( bvhvec3( 0, 30, -1 ), 9, 5, 0x1ffffff ); // hard-coded light source
	// build bvh (here: 'compressed wide bvh', for efficient GPU rendering)
	if (!bvh.Load( "cwbvh.bin" ))
	{
		// optimizing a BVH: from BVH to BVH_Verbose, optimize, then back to BVH.
		BVH bvh2;
		bvh2.Build( tris, triCount );
		BVH_Verbose verbose;
		verbose.ConvertFrom( bvh2 );
		verbose.Optimize( 1000000 ); // this will take a while: Next time, use cache.
		bvh2.ConvertFrom( verbose );
		// building a cwbvh without the convenient constructor: From BVH, via BVH8.
		BVH8 bvh8;
		bvh8.ConvertFrom( bvh2 );
		bvh.ConvertFrom( bvh8 );
		bvh.Save( "cwbvh.bin" );
	}
	// create OpenCL buffers for BVH data
	cwbvhNodes = new Buffer( bvh.usedBlocks * sizeof( bvhvec4 ), bvh.bvh8Data );
	cwbvhTris = new Buffer( bvh.idxCount * 3 * sizeof( bvhvec4 ), bvh.bvh8Tris );
	cwbvhNodes->CopyToDevice();
	cwbvhTris->CopyToDevice();
	triData = new Buffer( triCount * 3 * sizeof( bvhvec4 ), tris );
	triData->CopyToDevice();
	// load camera position / direction from file
	std::fstream t = std::fstream{ "camera_gpu.bin", t.binary | t.in };
	if (!t.is_open()) return;
	t.read( (char*)&rd, sizeof( rd ) );
}

// Keyboard handling
bool UpdateCamera( float delta_time_s, fenster& f )
{
	bvhvec3 right = normalize( cross( bvhvec3( 0, 1, 0 ), rd.view ) ), up = 0.8f * cross( rd.view, right );
	// get camera controls.
	float moved = 0, spd = 10.0f * delta_time_s;
	if (f.keys['A'] || f.keys['D']) rd.eye += right * (f.keys['D'] ? spd : -spd), moved = 1;
	if (f.keys['W'] || f.keys['S']) rd.eye += rd.view * (f.keys['W'] ? spd : -spd), moved = 1;
	if (f.keys['R'] || f.keys['F']) rd.eye += up * 2.0f * (f.keys['R'] ? spd : -spd), moved = 1;
	if (f.keys[20]) rd.view = normalize( rd.view + right * -0.1f * spd ), moved = 1;
	if (f.keys[19]) rd.view = normalize( rd.view + right * 0.1f * spd ), moved = 1;
	if (f.keys[17]) rd.view = normalize( rd.view + up * -0.1f * spd ), moved = 1;
	if (f.keys[18]) rd.view = normalize( rd.view + up * 0.1f * spd ), moved = 1;
	// recalculate right, up
	right = normalize( cross( bvhvec3( 0, 1, 0 ), rd.view ) ), up = 0.8f * cross( rd.view, right );
	bvhvec3 C = rd.eye + 1.2f * rd.view;
	rd.p0 = C - right + up, rd.p1 = C + right + up, rd.p2 = C - right - up;
	return moved > 0;
}

// Application Tick
void Tick( float delta_time_s, fenster& f, uint32_t* buf )
{
	// handle user input and update camera
	int N = SCRWIDTH * SCRHEIGHT;
	if (UpdateCamera( delta_time_s, f ) || frameIdx++ == 0)
	{
		clear->SetArguments( accumulator );
		clear->Run( N );
		spp = 1;
	}
	// wavefront step 0: render on the GPU
	init->SetArguments( N, rd.eye, rd.p0, rd.p1, rd.p2, frameIdx, SCRWIDTH, SCRHEIGHT, cwbvhNodes, cwbvhTris, noise );
	init->Run( 1 ); // init atomic counters, set buffer ptrs etc.
	rayGen->SetArguments( raysOut, spp * 19191 );
	rayGen->Run2D( oclint2( SCRWIDTH, SCRHEIGHT ) );
	for (int i = 0; i < 3; i++)
	{
		swap( raysOut, raysIn );
		extend->SetArguments( raysIn );
		extend->Run( computeUnits * 64 * 16, 64 );
		updateCounters1->Run( 1 );
		shade->SetArguments( accumulator, raysIn, raysOut, connections, triData, spp - 1 );
		shade->Run( computeUnits * 64 * 16, 64 );
		updateCounters2->Run( 1 );
	}
	traceShadows->SetArguments( accumulator, connections );
	traceShadows->Run( computeUnits * 64 * 8, 64 );
	finalize->SetArguments( accumulator, 1.0f / (float)spp++, pixels );
	finalize->Run2D( oclint2( SCRWIDTH, SCRHEIGHT ) );
	pixels->CopyFromDevice();
	memcpy( buf, pixels->GetHostPtr(), N * sizeof( uint32_t ) );
	// print frame time / rate in window title
	char title[50];
	sprintf( title, "tiny_bvh %.2f s %.2f Hz", delta_time_s, 1.0f / delta_time_s );
	fenster_update_title( &f, title );
}

// Application Shutdown
void Shutdown()
{
	// save camera position / direction to file
	std::fstream s = std::fstream{ "camera_gpu.bin", s.binary | s.out };
	s.write( (char*)&rd, sizeof( rd ) );
}