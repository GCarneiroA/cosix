#include <stdio.h>
#include <stdlib.h>
#include <program.h>
#include <argdata.h>
#include <argdata.hpp>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sched.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <sstream>
#include <sys/socket.h>
#include <sys/capsicum.h>
#include <cloudabi_syscalls.h>

#include <cosix/networkd.hpp>
#include <cosix/reverse.hpp>
#include <cosix/util.hpp>

#include <flower/switchboard/configuration.ad.h>
#include <flower_test/configuration.ad.h>

using namespace arpc;
using namespace cosix;

int stdout;
int procfs;
int bootfs;
int initrd;
int ifstore;
int termstore;
int blockdevstore;
std::shared_ptr<FileDescriptor> flower_switchboard;

struct cmdlinefs {
	std::string filesystem;
	std::string blockdev;
	int deviceid;
};

cmdlinefs configfs_spec;
int last_deviceid = 0;
int configfs = -1;
int tmpfs = -1;

static cmdlinefs parse_cmdlinefs(std::string value) {
	cmdlinefs res;
	auto at = value.find('@');
	if(at == std::string::npos) {
		res.blockdev = value;
	} else {
		res.filesystem = value.substr(0, at);
		res.blockdev = value.substr(at + 1);
	}
	res.deviceid = ++last_deviceid;
	return res;
}

static void parse_cmdline_key(std::string key, std::string value) {
	if(key == "cosix.config") {
		configfs_spec = parse_cmdlinefs(value);
	} else {
		// ignore key, it's not for us
	}
}

static void parse_cmdline(std::string cmdline) {
	std::string key, value;
	bool in_key = true;
	for(size_t i = 0; i <= cmdline.size(); ++i) {
		if(i == cmdline.size() || cmdline[i] == ' ') {
			if(!key.empty()) {
				// "foo=bar" or "foo" (value would be empty)
				parse_cmdline_key(key, value);
				key.clear();
				value.clear();
				in_key = true;
			}
		} else if(in_key && cmdline[i] == '=') {
			in_key = false;
		} else if(in_key) {
			key += cmdline[i];
		} else {
			value += cmdline[i];
		}
	}
}

long uptime() {
	int uptimefd = openat(procfs, "kernel/uptime", O_RDONLY);
	if(uptimefd < 0) {
		dprintf(stdout, "INIT: failed to open uptime: %s\n", strerror(errno));
		return 0;
	}
	char buf[16];
	ssize_t r = read(uptimefd, buf, sizeof(buf) - 1);
	if(r <= 0) {
		dprintf(stdout, "INIT: failed to read uptime: %s\n", strerror(errno));
		return 0;
	}
	buf[r] = 0;
	close(uptimefd);
	return atol(buf);
}

std::string read_cmdline() {
	std::string res;
	int cmdlinefd = openat(procfs, "kernel/cmdline", O_RDONLY);
	if(cmdlinefd < 0) {
		return res;
	}
	while(true) {
		char buf[64];
		ssize_t r = read(cmdlinefd, buf, sizeof(buf));
		if(r < 0) {
			res.clear();
			return res;
		} else if(r == 0) {
			return res;
		}
		res += std::string(buf, r);
	}

	close(cmdlinefd);
	return res;
}

argdata_t *argdata_create_string(const char *value) {
	return argdata_create_str(value, strlen(value));
}

int program_run(const char *name, int bfd, argdata_t const *ad) {
	auto *pd2 = program_spawn2(bfd, ad);
	if(pd2 == nullptr) {
		dprintf(stdout, "INIT: %s failed to start: %s\n", name, strerror(errno));
		return -1;
	}

	dprintf(stdout, "INIT: %s started.\n", name);

	int64_t exit_status;
	program_wait2(pd2, &exit_status, nullptr);
	dprintf(stdout, "INIT: %s exited, exit status %lld\n", name, exit_status);
	dprintf(stdout, "INIT: current uptime: %ld seconds\n", uptime());

	return exit_status;
}

int get_blockdev(std::string blockdev) {
	std::string command = "FD " + blockdev;
	if(write(blockdevstore, command.c_str(), command.size()) != static_cast<ssize_t>(command.size())) {
		perror("INIT: Failed to send command to blockdevstore");
		return -1;
	}
	char buf[20];
	int fdnum;
	if(cosix::read_response_and_fd(blockdevstore, buf, sizeof(buf), fdnum) != 2
	|| strncmp(buf, "OK", 2) != 0) {
		perror("INIT: Failed to retrieve blockdev device from blockdevstore");
		return -1;
	}
	return fdnum;
}

static int copy_userlandsock(int fd) {
	write(fd, "COPY", 4);
	char buf[20];
	int fdnum;
	if(cosix::read_response_and_fd(fd, buf, sizeof(buf), fdnum) != 2
	|| strncmp(buf, "OK", 2) != 0) {
		perror("Failed to retrieve copy from userlandsock");
		exit(1);
	}
	return fdnum;
}

static int copy_ifstorefd() {
	return copy_userlandsock(ifstore);
}

static int copy_blockdevstore() {
	return copy_userlandsock(blockdevstore);
}

std::shared_ptr<FileDescriptor> copy_switchboard() {
	using namespace flower::protocol::switchboard;

	auto channel = CreateChannel(flower_switchboard);
	auto stub = Switchboard::NewStub(channel);
	ClientContext context;
	ConstrainRequest request;
	// TODO: add a safer way to add all rights
	for(size_t i = Right_MIN; i <= Right_MAX; ++i) {
		Right r;
		if(Right_IsValid(i) && Right_Parse(Right_Name(i), &r)) {
			request.add_rights(r);
		}
	}
	ConstrainResponse response;
	if (Status status = stub->Constrain(&context, request, &response); !status.ok()) {
		dprintf(stdout, "init: Failed to constrain switchboard socket: %s\n", status.error_message().c_str());
		exit(1);
	}

	auto connection = response.switchboard();
	if(!connection) {
		dprintf(stdout, "init: switchboard did not return a connection\n");
		exit(1);
	}
	return connection;
}

void start_networkd() {
	int new_ifstorefd = copy_ifstorefd();
	auto switchboard = copy_switchboard();

	int bfd = openat(bootfs, "networkd", O_RDONLY);
	if(bfd < 0) {
		dprintf(stdout, "Can't run networkd, because it failed to open: %s\n", strerror(errno));
		return;
	}

	dprintf(stdout, "Running networkd...\n");
	argdata_t *keys[] = {argdata_create_string("stdout"), argdata_create_string("bootfs"), argdata_create_string("ifstore"), argdata_create_string("switchboard_socket")};
	argdata_t *values[] = {argdata_create_fd(stdout), argdata_create_fd(bootfs), argdata_create_fd(new_ifstorefd), argdata_create_fd(switchboard->get())};
	argdata_t *ad = argdata_create_map(keys, values, sizeof(keys) / sizeof(keys[0]));

	auto *pd2 = program_spawn2(bfd, ad);
	if(pd2 == nullptr) {
		dprintf(stdout, "networkd failed to spawn: %s\n", strerror(errno));
	} else {
		dprintf(stdout, "networkd spawned\n");
	}
}

int start_networked_binary(const char *name, int port, bool wait = true) {
	int new_ifstorefd = copy_ifstorefd();

	int shellfd = openat(bootfs, "pythonshell", O_RDONLY);
	if(shellfd < 0) {
		fprintf(stderr, "Failed to open shell: %s\n", strerror(errno));
		return 1;
	}

	int bfd = openat(bootfs, name, O_RDONLY);
	if(bfd < 0) {
		dprintf(stdout, "Failed to open %s: %s\n", name, strerror(errno));
		return 1;
	}

	dprintf(stdout, "Init going to program_spawn() %s...\n", name);

	int networkfd = cosix::networkd::open(flower_switchboard);

	auto switchboard = copy_switchboard();

	argdata_t *keys[] = {argdata_create_string("stdout"),
		argdata_create_string("tmpdir"),
		argdata_create_string("initrd"),
		argdata_create_string("networkd"),
		argdata_create_string("procfs"),
		argdata_create_string("bootfs"),
		argdata_create_string("port"),
		argdata_create_string("shell"),
		argdata_create_string("ifstore"),
		argdata_create_string("switchboard_socket"),
	};
	argdata_t *values[] = {argdata_create_fd(stdout),
		argdata_create_fd(configfs),
		argdata_create_fd(initrd),
		argdata_create_fd(networkfd),
		argdata_create_fd(procfs),
		argdata_create_fd(bootfs),
		argdata_create_int(port),
		argdata_create_fd(shellfd),
		argdata_create_fd(new_ifstorefd),
		argdata_create_fd(switchboard->get()),
	};
	argdata_t *ad = argdata_create_map(keys, values, sizeof(keys) / sizeof(keys[0]));

	if(wait) {
		auto r = program_run(name, bfd, ad);
		close(bfd);
		return r;
	} else {
		auto *pd2 = program_spawn2(bfd, ad);
		if(pd2 == nullptr) {
			dprintf(stdout, "%s failed to spawn: %s\n", name, strerror(errno));
			return 1;
		}
		return 0;
	}
}

void rm_rf_contents(DIR *d) {
	struct dirent *ent;
	std::vector<std::string> files;
	std::vector<std::string> directories;
	while((ent = readdir(d)) != nullptr) {
		if(ent->d_type == DT_DIR) {
			if(strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
				directories.push_back(ent->d_name);
			}
		} else {
			files.push_back(ent->d_name);
		}
	}

	for(auto &dir : directories) {
		// delete all files within
		int innerdh = openat(dirfd(d), dir.c_str(), O_RDONLY);
		DIR *innerdir = fdopendir(innerdh);
		rm_rf_contents(innerdir);
		closedir(innerdir);
		unlinkat(dirfd(d), dir.c_str(), AT_REMOVEDIR);
	}
	for(auto &f : files) {
		unlinkat(dirfd(d), f.c_str(), 0);
	}
}

static int mount_filesystem(cmdlinefs spec) {
	int blockdev = -1;
	if(!spec.blockdev.empty()) {
		blockdev = get_blockdev(spec.blockdev);
	}

	auto pseudopair = cosix::open_pseudo(ifstore, CLOUDABI_FILETYPE_DIRECTORY);

	// Limit pseudopair to things commonly allowed by a filesystem
	cloudabi_fdstat_t fsb = {
		.fs_rights_base =
			CLOUDABI_RIGHT_POLL_FD_READWRITE |
			CLOUDABI_RIGHT_FD_WRITE |
			CLOUDABI_RIGHT_FD_READ |
			CLOUDABI_RIGHT_SOCK_SHUTDOWN,
		.fs_rights_inheriting = 0,
	};
	errno = cloudabi_sys_fd_stat_put(pseudopair.first, &fsb, CLOUDABI_FDSTAT_RIGHTS);
	if(errno != 0) {
		perror("INIT: Failed to limit rights on reverse FD");
	}

	fsb.fs_rights_base =
		CLOUDABI_RIGHT_FILE_ALLOCATE |
		CLOUDABI_RIGHT_FILE_CREATE_DIRECTORY |
		CLOUDABI_RIGHT_FILE_CREATE_FILE |
		CLOUDABI_RIGHT_FILE_LINK_SOURCE |
		CLOUDABI_RIGHT_FILE_LINK_TARGET |
		CLOUDABI_RIGHT_FILE_OPEN |
		CLOUDABI_RIGHT_FILE_READDIR |
		CLOUDABI_RIGHT_FILE_READLINK |
		CLOUDABI_RIGHT_FILE_RENAME_SOURCE |
		CLOUDABI_RIGHT_FILE_RENAME_TARGET |
		CLOUDABI_RIGHT_FILE_STAT_FGET |
		CLOUDABI_RIGHT_FILE_STAT_GET |
		CLOUDABI_RIGHT_FILE_SYMLINK |
		CLOUDABI_RIGHT_FILE_UNLINK;
	fsb.fs_rights_inheriting =
		CLOUDABI_RIGHT_FD_DATASYNC |
		CLOUDABI_RIGHT_FD_READ |
		CLOUDABI_RIGHT_FD_SEEK |
		CLOUDABI_RIGHT_FD_STAT_PUT_FLAGS |
		CLOUDABI_RIGHT_FD_SYNC |
		CLOUDABI_RIGHT_FD_TELL |
		CLOUDABI_RIGHT_FD_WRITE |
		CLOUDABI_RIGHT_FILE_ADVISE |
		CLOUDABI_RIGHT_FILE_ALLOCATE |
		CLOUDABI_RIGHT_FILE_CREATE_DIRECTORY |
		CLOUDABI_RIGHT_FILE_CREATE_FILE |
		CLOUDABI_RIGHT_FILE_LINK_SOURCE |
		CLOUDABI_RIGHT_FILE_LINK_TARGET |
		CLOUDABI_RIGHT_FILE_OPEN |
		CLOUDABI_RIGHT_FILE_READDIR |
		CLOUDABI_RIGHT_FILE_READLINK |
		CLOUDABI_RIGHT_FILE_RENAME_SOURCE |
		CLOUDABI_RIGHT_FILE_RENAME_TARGET |
		CLOUDABI_RIGHT_FILE_STAT_FGET |
		CLOUDABI_RIGHT_FILE_STAT_FPUT_SIZE |
		CLOUDABI_RIGHT_FILE_STAT_FPUT_TIMES |
		CLOUDABI_RIGHT_FILE_STAT_GET |
		CLOUDABI_RIGHT_FILE_STAT_PUT_TIMES |
		CLOUDABI_RIGHT_FILE_SYMLINK |
		CLOUDABI_RIGHT_FILE_UNLINK |
		CLOUDABI_RIGHT_MEM_MAP |
		CLOUDABI_RIGHT_MEM_MAP_EXEC |
		CLOUDABI_RIGHT_POLL_FD_READWRITE |
		CLOUDABI_RIGHT_PROC_EXEC;
	errno = cloudabi_sys_fd_stat_put(pseudopair.second, &fsb, CLOUDABI_FDSTAT_RIGHTS);
	if(errno != 0) {
		perror("INIT: Failed to limit rights on pseudo FD");
	}

	// Run a filesystem driver and give it the reverse FD and block device
	std::string binary = spec.filesystem;
	if(spec.filesystem == "ext2") {
		binary = "extfs";
	}

	int bfd = openat(bootfs, binary.c_str(), O_RDONLY);
	if(bfd < 0) {
		fprintf(stderr, "INIT: Can't run filesystem driver %s, because it failed to open: %s\n", binary.c_str(), strerror(errno));
		close(pseudopair.first);
		close(pseudopair.second);
		close(blockdev);
		return -1;
	}

	argdata_t *keys[] = {
		argdata_create_string("stdout"),
		argdata_create_string("reversefd"),
		argdata_create_string("deviceid"),
		argdata_create_string("blockdev"),
	};
	auto *blockdev_ad = blockdev < 0 ? &argdata_null : argdata_create_fd(blockdev);
	argdata_t const *values[] = {
		argdata_create_fd(stdout),
		argdata_create_fd(pseudopair.first),
		argdata_create_int(spec.deviceid),
		blockdev_ad,
	};
	argdata_t *ad = argdata_create_map(keys, values, sizeof(keys) / sizeof(keys[0]));

	auto *pd2 = program_spawn2(bfd, ad);
	close(pseudopair.first);
	close(blockdev);
	close(bfd);
	if(pd2 == nullptr) {
		fprintf(stderr, "INIT: Filesystem driver %s failed to spawn: %s\n", binary.c_str(), strerror(errno));
		close(pseudopair.second);
		return -1;
	}

	// Return the pseudo FD
	return pseudopair.second;
}

void run_pseudotest() {
	int new_ifstorefd = copy_ifstorefd();
	int bfd = openat(bootfs, "pseudo_test", O_RDONLY);
	if(bfd < 0) {
		dprintf(stdout, "Can't run pseudo_test, because it failed to open: %s\n", strerror(errno));
		return;
	}

	dprintf(stdout, "Running pseudo_test...\n");
	argdata_t *keys[] = {argdata_create_string("stdout"), argdata_create_string("ifstore")};
	argdata_t *values[] = {argdata_create_fd(stdout), argdata_create_fd(new_ifstorefd)};
	argdata_t *ad = argdata_create_map(keys, values, sizeof(keys) / sizeof(keys[0]));

	auto *pd2 = program_spawn2(bfd, ad);
	if(pd2 == nullptr) {
		dprintf(stdout, "pseudo_test failed to spawn: %s\n", strerror(errno));
	} else {
		dprintf(stdout, "pseudo_test spawned\n");
	}
}

void run_ircclient(int consolefd) {
	int networkd = cosix::networkd::open(flower_switchboard);
	//int ircd = cosix::networkd::get_socket(networkd, SOCK_STREAM, "138.201.39.100:6667", ""); // kassala
	//int ircd = cosix::networkd::get_socket(networkd, SOCK_STREAM, "193.163.220.3:6667", ""); // efnet
	int ircd = cosix::networkd::get_socket(networkd, SOCK_STREAM, "71.11.84.232:6667", ""); // freenode
	close(networkd);

	int bfd = openat(bootfs, "ircclient", O_RDONLY);
	if(bfd < 0) {
		dprintf(stdout, "Can't run ircclient, because it failed to open: %s\n", strerror(errno));
		exit(1);
	}

	dprintf(stdout, "Running ircclient...\n");
	argdata_t *keys[] = {argdata_create_string("terminal"), argdata_create_string("ircd"), argdata_create_string("nick")};
	argdata_t *values[] = {argdata_create_fd(consolefd), argdata_create_fd(ircd), argdata_create_string("cosix")};
	argdata_t *ad = argdata_create_map(keys, values, sizeof(keys) / sizeof(keys[0]));
	program_run("ircclient", bfd, ad);
}

void run_pythonshell(int consolefd) {
	int bfd = openat(bootfs, "pythonshell", O_RDONLY);
	if(bfd < 0) {
		fprintf(stderr, "Can't run pythonshell, because it failed to open: %s\n", strerror(errno));
		exit(1);
	}

	int networkd = cosix::networkd::open(flower_switchboard);
	argdata_t *keys[] = {argdata_create_string("terminal"),
		argdata_create_string("networkd"),
		argdata_create_string("procfs"),
		argdata_create_string("bootfs"),
		argdata_create_string("tmpdir"),
		argdata_create_string("initrd")};
	argdata_t *values[] = {argdata_create_fd(consolefd),
		argdata_create_fd(networkd),
		argdata_create_fd(procfs),
		argdata_create_fd(bootfs),
		argdata_create_fd(configfs),
		argdata_create_fd(initrd)};
	argdata_t *ad = argdata_create_map(keys, values, sizeof(keys) / sizeof(keys[0]));
	program_run("pythonshell", bfd, ad);
	close(networkd);
}

static void create_partitions_on_block_device(std::string blockdev) {
	int bfd = openat(bootfs, "partition", O_RDONLY);
	if(bfd < 0) {
		fprintf(stderr, "INIT: Can't create partitions, because the partition tool failed to open: %s\n", strerror(errno));
		exit(1);
	}

	int new_bdevfd = copy_blockdevstore();
	argdata_t *keys[] = {argdata_create_string("stdout"),
		argdata_create_string("blockdevstore"),
		argdata_create_string("blockdev")};
	argdata_t *values[] = {argdata_create_fd(stdout),
		argdata_create_fd(new_bdevfd),
		argdata_create_string(blockdev.c_str())};
	argdata_t *ad = argdata_create_map(keys, values, sizeof(keys) / sizeof(keys[0]));
	program_run("partition", bfd, ad);
	close(new_bdevfd);
}

static std::vector<std::string> all_block_devices() {
	if(write(blockdevstore, "LIST", 4) != 4) {
		dprintf(stdout, "INIT: Failed to send ATA list command: %s\n", strerror(errno));
		exit(1);
	}

	char buf[1024];
	ssize_t rd = read(blockdevstore, buf, sizeof(buf) - 1);
	if(rd < 0) {
		dprintf(stdout, "INIT: Failed to receive ATA list result: %s\n", strerror(errno));
		exit(1);
	}
	buf[rd] = 0;

	char *line_ptr = buf;
	std::vector<std::string> res;
	while(line_ptr && line_ptr[0] != 0) {
		char *next_line = strchr(line_ptr, '\n');
		if(next_line) {
			res.push_back(std::string(line_ptr, next_line - line_ptr));
			line_ptr = next_line + 1;
		} else {
			res.push_back(line_ptr);
			line_ptr = nullptr;
		}
	}
	return res;
}

void program_main(const argdata_t *) {
	stdout = 0;
	procfs = 2;
	bootfs = 3;
	initrd = 4;
	ifstore = 5;
	termstore = 6;
	blockdevstore = 7;

	dprintf(stdout, "INIT: Starting up.\n");

	// reconfigure stderr
	FILE *out = fdopen(stdout, "w");
	setvbuf(out, nullptr, _IONBF, BUFSIZ);
	fswap(stderr, out);

	std::string cmdline = read_cmdline();
	dprintf(stdout, "INIT: cmdline: %s\n", cmdline.c_str());
	parse_cmdline(cmdline);

	dprintf(stdout, "INIT: Creating partitions on all block devices...\n");

	for(auto const &blockdev : all_block_devices()) {
		create_partitions_on_block_device(blockdev);
	}

	dprintf(stdout, "INIT: Partitions created; waiting for blockdev %s, mounting as FS %s\n", configfs_spec.blockdev.c_str(), configfs_spec.filesystem.c_str());

	bool found = false;
	int wait_times = 10;
	while(!found) {
		for(auto const &blockdev : all_block_devices()) {
			if(blockdev == configfs_spec.blockdev) {
				found = true;
				break;
			}
		}
		if(!found) {
			if(wait_times-- == 0) {
				fprintf(stderr, "INIT: Timed out waiting for block device to appear. Exiting.\n");
				exit(1);
			}
			// wait for a bit for the partition to be found
			struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
			clock_nanosleep(CLOCK_MONOTONIC, 0, &ts);
		}
	}

	dprintf(stdout, "INIT: Mounting config filesystem...\n");
	configfs = mount_filesystem(configfs_spec);

	// open a tmpfs too, if necessary
	cmdlinefs tmpfs_spec;
	tmpfs_spec.filesystem = "tmpfs";
	tmpfs_spec.deviceid = ++last_deviceid;
	tmpfs = mount_filesystem(tmpfs_spec);

	int fds[2];
	if(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		perror("socketpair");
		exit(1);
	}

	auto initial_channel = std::make_shared<FileDescriptor>(fds[0]);
	flower_switchboard = std::make_shared<FileDescriptor>(fds[1]);

	{
		auto logger_output = std::make_shared<FileDescriptor>(dup(stdout));

		flower::switchboard::Configuration flowerconf;
		flowerconf.set_initial_channel(initial_channel);
		flowerconf.set_logger_output(logger_output);
		flowerconf.set_worker_pool_size(16);

		int bfd = openat(initrd, "bin/flower_switchboard", O_RDONLY);
		if(bfd < 0) {
			dprintf(stdout, "Can't run Flower switchboard, because it failed to open: %s\n", strerror(errno));
			exit(1);
		}
		dprintf(stdout, "Running Flower switchboard...\n");
		arpc::ArgdataBuilder builder;
		auto *pd2 = program_spawn2(bfd, flowerconf.Build(&builder));
		if(pd2 == nullptr) {
			dprintf(stdout, "Switchboard failed to spawn: %s\n", strerror(errno));
			exit(1);
		} else {
			dprintf(stdout, "Switchboard spawned\n");
		}
	}

	{
		auto logger_output = std::make_shared<FileDescriptor>(dup(stdout));

		cosix::flower_test::Configuration testconf;
		testconf.set_switchboard_socket(copy_switchboard());
		testconf.set_logger_output(logger_output);

		int bfd = openat(bootfs, "flower_test", O_RDONLY);
		if(bfd < 0) {
			dprintf(stdout, "Failed to run flower_test, because it failed to open: %s\n", strerror(errno));
			exit(1);
		}
		arpc::ArgdataBuilder builder;
		program_run("flower_test", bfd, testconf.Build(&builder));
	}

	start_networkd();

	// sleep for a bit for networkd to come up
	{
		struct timespec ts = {.tv_sec = 5, .tv_nsec = 0};
		clock_nanosleep(CLOCK_MONOTONIC, 0, &ts);
	}

	run_pseudotest();
	start_networked_binary("httpd", 80, false);
	start_networked_binary("telnetd", 26, false);

	int consolefd = openat(termstore, "console", O_RDWR);
	if(consolefd < 0) {
		dprintf(stdout, "Failed to open consolefd: %s\n", strerror(errno));
		exit(1);
	}

	run_pythonshell(consolefd);
	run_ircclient(consolefd);

	pthread_mutex_t mtx;
	pthread_mutex_init(&mtx, NULL);
	pthread_cond_t cond;
	pthread_cond_init(&cond, NULL);
	pthread_mutex_lock(&mtx);
	pthread_cond_wait(&cond, &mtx);
	pthread_mutex_unlock(&mtx);
	exit(0);

	// 1. Open the init-binaries directory fd from argdata
	// 2. Read some configuration from the kernel cmdline
	// 3. Start the necessary applications (like dhcpcd)
	// 4. Keep track of their status using poll()
	//    (so that the application actually always blocks)
	// 5. If needed, open an init RPC socket so that applications or the
	//    kernel can always ask for extra services
}
