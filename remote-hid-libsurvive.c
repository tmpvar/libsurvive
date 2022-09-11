#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")

#include <os_generic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <survive_api.h>

#include <xxhash.h>

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

#pragma pack(push, 1)
struct FullState {
	struct {
		f32 x;
		f32 y;
		f32 z;
	} pos;
	struct {
		f32 x;
		f32 y;
		f32 z;
		f32 w;
	} rot;
	u16 buttonsCount;
	u16 analogsCount;
	u16 joysticksCount;

	u32 buttons;
	// f32 analogs[HOTCART_REMOTE_HID_ANALOGS_MAX];
	// struct { f32 x, f32 y } joysticks[HOTCART_REMOTE_HID_JOYSTICKS_MAX];
};

struct LogEntry {
	u16 length;
	char msg[512];
};

struct RemoteHIDPacket {
	u16 type;		// RemoteHIDPacketType
	u16 deviceType; // RemoteHIDDeviceType
	u64 deviceID;	// hashed value

	union {
		struct FullState fullState;
		struct LogEntry logEntry;
	};
};
#pragma pack(pop)

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

	SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == INVALID_SOCKET) {
		printf("Could not create socket : %d", WSAGetLastError());
		return 1;
	}

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(HOTCART_PORT);
	addr.sin_addr.s_addr = inet_addr("192.168.0.255");

	char broadcast = 1;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
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

	struct SurviveSimpleEvent event = {0};

	u32 buttonStates = 0;
	while (keepRunning && survive_simple_wait_for_event(actx, &event) != SurviveSimpleEventType_Shutdown) {
		switch (event.event_type) {
		case SurviveSimpleEventType_PoseUpdateEvent: {
			const struct SurviveSimplePoseUpdatedEvent *pose_event = survive_simple_get_pose_updated_event(&event);
			SurvivePose pose = pose_event->pose;
			FLT timecode = pose_event->time;
			// printf("%s %s (%7.3f): %f %f %f %f %f %f %f\n", survive_simple_object_name(pose_event->object),
			// 	   survive_simple_serial_number(pose_event->object), timecode, pose.Pos[0], pose.Pos[1], pose.Pos[2],
			// 	   pose.Rot[0], pose.Rot[1], pose.Rot[2], pose.Rot[3]);

			struct RemoteHIDPacket packet;
			memset(&packet, 0, sizeof(packet));
			packet.type = 1;	   // FullState
			packet.deviceType = 2; // SixDOF
			char *serial = survive_simple_serial_number(pose_event->object);
			packet.deviceID = XXH64(serial, strlen(serial), 0);
			// Swizzle to y-up
			packet.fullState.pos.x = pose.Pos[0];
			packet.fullState.pos.y = pose.Pos[2];
			packet.fullState.pos.z = pose.Pos[1];

			packet.fullState.rot.x = pose.Rot[0];
			packet.fullState.rot.y = pose.Rot[1];
			packet.fullState.rot.z = pose.Rot[2];
			packet.fullState.rot.w = pose.Rot[3];

			packet.fullState.buttonsCount = 1;
			packet.fullState.buttons = survive_simple_object_get_button_mask(pose_event->object);
			int sent = sendto(sock, (char *)&packet, sizeof(packet), 0, (struct sockaddr *)&addr, sizeof(addr));
			break;
		}
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
		}
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
