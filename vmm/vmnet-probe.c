/*
 * vmnet-probe.c — ask macOS's vmnet for an interface in each of its modes
 * and report which ones it will give. nvmm-run's network card sits on vmnet;
 * when that does not come up, this says whether the trouble is vmnet on this
 * machine or nvmm-run. Needs root.
 *
 *   sudo ./vmnet-probe [interface to bridge to, default en0]
 */

#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vmnet/vmnet.h>

static void
probe(const char *name, operating_modes_t mode, const char *bridge_to)
{
	dispatch_queue_t q = dispatch_queue_create("vmnet-probe", NULL);
	dispatch_semaphore_t sem = dispatch_semaphore_create(0);
	xpc_object_t desc = xpc_dictionary_create(NULL, NULL, 0);
	__block vmnet_return_t status = VMNET_FAILURE;
	static char mac[32], addr[64];
	interface_ref iface;

	mac[0] = addr[0] = '\0';
	xpc_dictionary_set_uint64(desc, vmnet_operation_mode_key, mode);
	if (bridge_to != NULL) {
		if (__builtin_available(macOS 10.15, *))
			xpc_dictionary_set_string(desc,
			    vmnet_shared_interface_name_key, bridge_to);
	}

	printf("%-22s ", name);
	fflush(stdout);
	iface = vmnet_start_interface(desc, q,
	    ^(vmnet_return_t st, xpc_object_t params) {
		const char *s;

		status = st;
		if (st == VMNET_SUCCESS && params != NULL) {
			s = xpc_dictionary_get_string(params,
			    vmnet_mac_address_key);
			if (s != NULL)
				snprintf(mac, sizeof(mac), "%s", s);
			s = NULL;
			if (__builtin_available(macOS 10.15, *))
				s = xpc_dictionary_get_string(params,
				    vmnet_start_address_key);
			if (s != NULL)
				snprintf(addr, sizeof(addr), "%s", s);
		}
		dispatch_semaphore_signal(sem);
	});
	if (iface == NULL) {
		printf("refused at once (not root?)\n");
		return;
	}
	if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW,
	    15 * NSEC_PER_SEC)) != 0) {
		printf("NO ANSWER in 15 seconds\n");
		return;	/* leave it: stopping one that never started hangs too */
	}
	if (status != VMNET_SUCCESS) {
		printf("failed, vmnet status %d\n", (int)status);
		return;
	}
	printf("ok, MAC %s%s%s\n", mac, addr[0] ? ", host side " : "", addr);

	vmnet_stop_interface(iface, q, ^(vmnet_return_t st) {
		(void)st;
		dispatch_semaphore_signal(sem);
	});
	(void)dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW,
	    10 * NSEC_PER_SEC));
}

int
main(int argc, char **argv)
{
	const char *ifname = argc > 1 ? argv[1] : "en0";
	char label[64];

	if (geteuid() != 0)
		printf("warning: not root; vmnet will refuse\n");
	probe("host-only", VMNET_HOST_MODE, NULL);
	snprintf(label, sizeof(label), "bridged to %s", ifname);
	if (__builtin_available(macOS 10.15, *))
		probe(label, VMNET_BRIDGED_MODE, ifname);
	else
		printf("%-22s needs macOS 10.15\n", label);
	probe("shared (NAT)", VMNET_SHARED_MODE, NULL);
	return 0;
}
