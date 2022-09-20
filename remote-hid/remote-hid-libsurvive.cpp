#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")

#include <os_generic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <survive_api.h>

#include <xxhash.h>
#include "remote-hid_generated.h"
typedef unsigned long long u64;
typedef long long i64;
typedef unsigned int u32;
typedef int i32;
typedef unsigned short u16;
typedef short i16;
typedef unsigned char u8;
typedef char i8;
typedef float f32;
typedef double f64;

static volatile int keepRunning = 1;

#ifdef __linux__

#include <assert.h>
#include <signal.h>
#include <stdlib.h>

void intHandler(int dummy) {
	if (keepRunning == 0)
		exit(-1);
	keepRunning = 0;
}

#endif

static void log_fn(SurviveSimpleContext *actx, SurviveLogLevel logLevel, const char *msg) {
	fprintf(stderr, "(%7.3f) SimpleApi: %s\n", survive_simple_run_time(actx), msg);
}

#define HOTCART_PORT 49222

struct SocketState {
	SOCKET sock;
	struct sockaddr_in addr;
};

static void emit(SocketState *socketState, SurviveSimpleContext *ctx, const SurviveSimpleObject *object) {
	
	flatbuffers::FlatBufferBuilder builder(1024);

	SurvivePose pose;
	survive_simple_object_get_latest_pose(object, &pose);

	// swizzle to y+ up
	RemoteHID::v3 pos(pose.Pos[0], pose.Pos[2], pose.Pos[1]);

	// Swizzle wxyz (up=z+) -> xzyw (up=y+)
	RemoteHID::quat rot(pose.Rot[1], pose.Rot[3], pose.Rot[2], -pose.Rot[0]);

	u32 buttonMasks[1] = {u32(survive_simple_object_get_button_mask(object))};

	f32 analogs[1] = {
		f32(survive_simple_object_get_input_axis(object, SurviveAxis::SURVIVE_AXIS_TRIGGER)),
	};

	RemoteHID::v2 joysticks[1] = {
		RemoteHID::v2(survive_simple_object_get_input_axis(object, SURVIVE_AXIS_TRACKPAD_X),
					  survive_simple_object_get_input_axis(object, SURVIVE_AXIS_TRACKPAD_Y))};

	auto fullState = RemoteHID::CreatePayloadFullState(builder, &pos, &rot, builder.CreateVector(buttonMasks, 1),
													   builder.CreateVector(analogs, 1),
													   builder.CreateVectorOfStructs(joysticks, 1));

	const char *serial = survive_simple_serial_number(object);
	const u64 deviceID = XXH64(serial, strlen(serial), 0);

	auto packet = RemoteHID::CreatePacket(builder, RemoteHID::DeviceType::DeviceType_SixDOF, deviceID,
										  RemoteHID::Payloads_PayloadFullState, fullState.Union());

	builder.Finish(packet);
	// printf("%s %s (%7.3f): %f %f %f %f %f %f %f\n", survive_simple_object_name(pose_event->object),
	// 	   survive_simple_serial_number(pose_event->object), timecode, pose.Pos[0], pose.Pos[1], pose.Pos[2],
	// 	   pose.Rot[0], pose.Rot[1], pose.Rot[2], pose.Rot[3]);

	int sent = sendto(
	  socketState->sock,
	  (char *)builder.GetBufferPointer(),
	  builder.GetSize(),
	  0,
	  (struct sockaddr *)&socketState->addr,
	  sizeof(socketState->addr)
	);
}

int main(int argc, char **argv) {
#ifdef __linux__
	signal(SIGINT, intHandler);
	signal(SIGTERM, intHandler);
	signal(SIGKILL, intHandler);
#endif

#ifdef WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		printf("Failed. Error Code : %d", WSAGetLastError());
		exit(EXIT_FAILURE);
	}

	SocketState socketState;

	socketState.sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (socketState.sock == INVALID_SOCKET) {
		printf("Could not create socket : %d", WSAGetLastError());
		return 1;
	}

	socketState.addr.sin_family = AF_INET;
	socketState.addr.sin_addr.s_addr = INADDR_ANY;
	socketState.addr.sin_port = htons(HOTCART_PORT);
	socketState.addr.sin_addr.s_addr = inet_addr("192.168.0.255");

	char broadcast = 1;
	setsockopt(socketState.sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
#endif

	SurviveSimpleContext *actx = survive_simple_init_with_logger(argc, argv, log_fn);
	if (actx == 0) // implies -help or similiar
		return 0;

	double start_time = OGGetAbsoluteTime();
	survive_simple_start_thread(actx);

	for (const SurviveSimpleObject *it = survive_simple_get_first_object(actx); it != 0;
		 it = survive_simple_get_next_object(actx, it)) {
		printf("Found '%s'\n", survive_simple_object_name(it));
	}

	struct SurviveSimpleEvent event = {};

	u32 buttonStates = 0;
	while (keepRunning && survive_simple_wait_for_event(actx, &event) != SurviveSimpleEventType_Shutdown) {
		switch (event.event_type) {
		case SurviveSimpleEventType_PoseUpdateEvent: {
			const struct SurviveSimplePoseUpdatedEvent *pose_event = survive_simple_get_pose_updated_event(&event);
			emit(&socketState, actx, pose_event->object);
			break;
		}
		case SurviveSimpleEventType_ButtonEvent: {
			const struct SurviveSimpleButtonEvent *button_event = survive_simple_get_button_event(&event);
			emit(&socketState, actx, button_event->object);
			break;
		}

		/*
		case SurviveSimpleEventType_ButtonEvent: {
			const struct SurviveSimpleButtonEvent *button_event = survive_simple_get_button_event(&event);
			SurviveObjectSubtype subtype = survive_simple_object_get_subtype(button_event->object);
			printf("%s input %s (%d) ", survive_simple_object_name(button_event->object),
				   SurviveInputEventStr(button_event->event_type), button_event->event_type);

			FLT v1 = survive_simple_object_get_input_axis(button_event->object, SURVIVE_AXIS_TRACKPAD_X) / 2. + .5;

			if (button_event->button_id != 255) {
				printf(" button %16s (%2d) ", SurviveButtonsStr(subtype, button_event->button_id),
					   button_event->button_id);

				if (button_event->button_id == SURVIVE_BUTTON_SYSTEM) {
					FLT v = 1 - survive_simple_object_get_input_axis(button_event->object, SURVIVE_AXIS_TRIGGER);
					survive_simple_object_haptic(button_event->object, 30, v, .5);
				}
			}
			for (int i = 0; i < button_event->axis_count; i++) {
				printf(" %20s (%2d) %+5.4f   ", SurviveAxisStr(subtype, button_event->axis_ids[i]),
					   button_event->axis_ids[i], button_event->axis_val[i]);
			}
			printf("\n");
			break;
		}*/

		case SurviveSimpleEventType_ConfigEvent: {
			const struct SurviveSimpleConfigEvent *cfg_event = survive_simple_get_config_event(&event);
			printf("(%f) %s received configuration of length %u type %d-%d\n", cfg_event->time,
				   survive_simple_object_name(cfg_event->object), (unsigned)strlen(cfg_event->cfg),
				   survive_simple_object_get_type(cfg_event->object),
				   survive_simple_object_get_subtype(cfg_event->object));
			break;
		}
		case SurviveSimpleEventType_DeviceAdded: {
			const struct SurviveSimpleObjectEvent *obj_event = survive_simple_get_object_event(&event);
			printf("(%f) Found '%s'\n", obj_event->time, survive_simple_object_name(obj_event->object));
			break;
		}
		case SurviveSimpleEventType_None:
			break;
		}
	}

	printf("Cleaning up\n");
	survive_simple_close(actx);
	return 0;
}
