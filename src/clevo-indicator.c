/*
 ============================================================================
 Name        : clevo-indicator.c
 Author      : AqD <iiiaqd@gmail.com>
 Version     :
 Description : Ubuntu fan control indicator for Clevo laptops

 Based on http://www.association-apml.fr/upload/fanctrl.c by Jonas Diemer
 (diemer@gmx.de)

 ============================================================================
 Auto fan control algorithm:

 The algorithm is to replace the builtin auto fan-control algorithm in Clevo
 laptops which is apparently broken in recent models such as W350SSQ, where the
 fan doesn't get kicked until both of GPU and CPU are really hot (and GPU
 cannot be hot anymore thanks to nVIDIA's Maxwell chips). It's far more
 aggressive than the builtin algorithm in order to keep the temperatures below
 60°C all the time, for maximized performance with Intel turbo boost enabled.

 ============================================================================
 */

#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <libayatana-appindicator3-0.1/libayatana-appindicator/app-indicator.h>

#define NAME "clevo-indicator"

#define EC_SC 0x66
#define EC_DATA 0x62

#define IBF 1
#define OBF 0
#define EC_SC_READ_CMD 0x80

/* EC registers can be read by EC_SC_READ_CMD or /sys/kernel/debug/ec/ec0/io:
 *
 * 1. modprobe ec_sys
 * 2. od -Ax -t x1 /sys/kernel/debug/ec/ec0/io
 */

#define EC_REG_SIZE 0x100
#define EC_REG_CPU_TEMP 0x07
#define EC_REG_GPU_TEMP 0xCD
#define EC_REG_FAN1_DUTY 0xCE
#define EC_REG_FAN2_DUTY 0xCF
#define EC_REG_FAN1_RPMS_HI 0xD0
#define EC_REG_FAN1_RPMS_LO 0xD1
#define EC_REG_FAN2_RPMS_HI 0xD2
#define EC_REG_FAN2_RPMS_LO 0xD3

#define EC_FAN_CMD 0x99
#define EC_FAN_PORT_CPU 0x01
#define EC_FAN_PORT_GPU 0x02

#define MIN_FAN_DUTY 0
#define MAX_FAN_DUTY 100
#define INDICATOR_LABEL_GUIDE "C99 G99"
#define INDICATOR_ICON_FILE "fan.jpg"
#define INDICATOR_ICON_INSTALL_DIR "/usr/local/share/icons"

typedef enum {
    NA = 0,
    AUTO = 1,
    MANUAL_LINKED = 2,
    MANUAL_FAN1 = 3,
    MANUAL_FAN2 = 4
} MenuItemType;

static void main_init_share(void);
static int main_ec_worker(void);
static void main_ui_worker(int argc, char** argv);
static void main_on_sigchld(int signum);
static void main_on_sigterm(int signum);
static int main_dump_fan(void);
static int main_test_fan(int duty_percentage);
static int main_test_fan_dual(int fan1_duty, int fan2_duty);
static gboolean ui_update(gpointer user_data);
static void ui_clear_manual_requests(void);
static void ui_command_set_fan(long fan_duty);
static void ui_command_set_fan1(long fan_duty);
static void ui_command_set_fan2(long fan_duty);
static void ui_command_quit(gchar* command);
static void ui_toggle_menuitems(MenuItemType active_type, int active_duty);
static void ui_format_status_menu(char* buf, size_t size);
static int ui_build_icon_path(const char* theme_dir, char* icon_file,
        size_t icon_size);
static int ui_resolve_tray_icons(char* theme_path, size_t theme_size,
        char* icon_file, size_t icon_size);
static int ui_refresh_tray_icon(int force);
static int validate_fan_duty(int duty);
static void main_sanitize_cwd(void);
static int ec_load_ec_sys_module(void);
static void ec_on_sigterm(int signum);
static int ec_init(void);
static int ec_auto_duty_adjust_for_temp(int temp, int duty);
static int ec_query_cpu_temp(void);
static int ec_query_gpu_temp(void);
static int ec_query_fan1_duty(void);
static int ec_query_fan2_duty(void);
static int ec_query_fan1_rpms(void);
static int ec_query_fan2_rpms(void);
static int ec_write_fan1_duty(int duty_percentage);
static int ec_write_fan2_duty(int duty_percentage);
static int ec_write_fan_duty(int duty_percentage);
static int ec_duty_to_raw(int duty_percentage);
static int ec_io_wait(const uint32_t port, const uint32_t flag,
        const char value);
static uint8_t ec_io_read(const uint32_t port);
static int ec_io_do(const uint32_t cmd, const uint32_t port,
        const uint8_t value);
static int calculate_fan_duty(int raw_duty);
static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low);
static int check_proc_instances(const char* proc_name);
static void get_time_string(char* buffer, size_t max, const char* format);
static void signal_term(__sighandler_t handler);

static AppIndicator* indicator = NULL;
static GtkWidget* status_menu_label = NULL;
static char ui_icon_source[PATH_MAX];
static char ui_icon_published[PATH_MAX];
static time_t ui_icon_published_mtime;

struct {
    char label[256];
    GCallback callback;
    long option;
    MenuItemType type;
    GtkWidget* widget;
} static menuitems[] = {
        { "Both Fans AUTO (CPU/GPU)", G_CALLBACK(ui_command_set_fan), 0, AUTO, NULL },
        { "", NULL, 0, NA, NULL },
        { "Both Fans  60%", G_CALLBACK(ui_command_set_fan), 60, MANUAL_LINKED, NULL },
        { "Both Fans  70%", G_CALLBACK(ui_command_set_fan), 70, MANUAL_LINKED, NULL },
        { "Both Fans  80%", G_CALLBACK(ui_command_set_fan), 80, MANUAL_LINKED, NULL },
        { "Both Fans  90%", G_CALLBACK(ui_command_set_fan), 90, MANUAL_LINKED, NULL },
        { "Both Fans 100%", G_CALLBACK(ui_command_set_fan), 100, MANUAL_LINKED, NULL },
        { "", NULL, 0, NA, NULL },
        { "CPU Fan  60%", G_CALLBACK(ui_command_set_fan1), 60, MANUAL_FAN1, NULL },
        { "CPU Fan  70%", G_CALLBACK(ui_command_set_fan1), 70, MANUAL_FAN1, NULL },
        { "CPU Fan  80%", G_CALLBACK(ui_command_set_fan1), 80, MANUAL_FAN1, NULL },
        { "CPU Fan  90%", G_CALLBACK(ui_command_set_fan1), 90, MANUAL_FAN1, NULL },
        { "CPU Fan 100%", G_CALLBACK(ui_command_set_fan1), 100, MANUAL_FAN1, NULL },
        { "", NULL, 0, NA, NULL },
        { "GPU Fan  60%", G_CALLBACK(ui_command_set_fan2), 60, MANUAL_FAN2, NULL },
        { "GPU Fan  70%", G_CALLBACK(ui_command_set_fan2), 70, MANUAL_FAN2, NULL },
        { "GPU Fan  80%", G_CALLBACK(ui_command_set_fan2), 80, MANUAL_FAN2, NULL },
        { "GPU Fan  90%", G_CALLBACK(ui_command_set_fan2), 90, MANUAL_FAN2, NULL },
        { "GPU Fan 100%", G_CALLBACK(ui_command_set_fan2), 100, MANUAL_FAN2, NULL },
        { "", NULL, 0, NA, NULL },
        { "Quit", G_CALLBACK(ui_command_quit), 0, NA, NULL }
};

static int menuitem_count = (sizeof(menuitems) / sizeof(menuitems[0]));

struct {
    volatile int exit;
    volatile int cpu_temp;
    volatile int gpu_temp;
    volatile int fan1_duty;
    volatile int fan2_duty;
    volatile int fan1_rpms;
    volatile int fan2_rpms;
    volatile int auto_duty;
    volatile int auto_duty_val_fan1;
    volatile int auto_duty_val_fan2;
    volatile int manual_next_fan1_duty;
    volatile int manual_next_fan2_duty;
    volatile int manual_prev_fan1_duty;
    volatile int manual_prev_fan2_duty;
}static *share_info = NULL;

static MenuItemType ui_active_type = AUTO;
static int ui_active_duty = 0;

static pid_t parent_pid = 0;

int main(int argc, char* argv[]) {
    main_sanitize_cwd();
    printf("Dual-fan control utility for Clevo laptops\n");
    if (check_proc_instances(NAME) > 1) {
        fprintf(stderr, "Multiple running instances of %s!\n", NAME);
        return EXIT_FAILURE;
    }
    if (ec_init() != EXIT_SUCCESS) {
        printf("unable to control EC: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    if (argc <= 1) {
        char* display = getenv("DISPLAY");
        if (display == NULL || strlen(display) == 0) {
            return main_dump_fan();
        } else {
            parent_pid = getpid();
            main_init_share();
            signal(SIGCHLD, &main_on_sigchld);
            signal_term(&main_on_sigterm);
            pid_t worker_pid = fork();
            if (worker_pid == 0) {
                signal(SIGCHLD, SIG_DFL);
                signal_term(&ec_on_sigterm);
                return main_ec_worker();
            } else if (worker_pid > 0) {
                main_ui_worker(argc, argv);
                share_info->exit = 1;
                waitpid(worker_pid, NULL, 0);
            } else {
                printf("unable to create worker: %s\n", strerror(errno));
                return EXIT_FAILURE;
            }
        }
    } else {
        if (argv[1][0] == '-') {
            printf(
                    "\n\
Usage: clevo-indicator [fan-duty-percentage]\n\
\n\
Dump/Control fans on Clevo laptops. Display indicator by default.\n\
\n\
Arguments:\n\
  [fan-duty-percentage]\t\tSet both fans to same duty (0-100)\n\
  [fan1] [fan2]\t\t\tSet CPU and GPU fan duty separately\n\
  -?\t\t\t\tDisplay this help and exit\n\
\n\
Without arguments this program should attempt to display an indicator in\n\
the Ubuntu tray area for fan information display and control. The indicator\n\
requires this program to have setuid=root flag but run from the desktop user\n\
, because a root user is not allowed to display a desktop indicator while a\n\
non-root user is not allowed to control Clevo EC (Embedded Controller that's\n\
responsible of the fan). Fix permissions of this executable if it fails to\n\
run:\n\
    sudo chown root clevo-indicator\n\
    sudo chmod u+s  clevo-indicator\n\
\n\
Note any fan duty change should take 1-2 seconds to come into effect - you\n\
can verify by the tray label or menu and also louder fan noise.\n\
\n\
In the indicator mode, this program would always attempt to load kernel\n\
module 'ec_sys', in order to query EC information from\n\
'/sys/kernel/debug/ec/ec0/io' instead of polling EC ports for readings,\n\
which may be more risky if interrupted or concurrently operated during the\n\
process.\n\
\n\
DO NOT MANIPULATE OR QUERY EC I/O PORTS WHILE THIS PROGRAM IS RUNNING.\n\
\n");
            return main_dump_fan();
        } else if (argc >= 3) {
            int fan1 = atoi(argv[1]);
            int fan2 = atoi(argv[2]);
            if (!validate_fan_duty(fan1) || !validate_fan_duty(fan2)) {
                printf("invalid fan duty! (valid range: %d-%d)\n", MIN_FAN_DUTY,
                        MAX_FAN_DUTY);
                return EXIT_FAILURE;
            }
            return main_test_fan_dual(fan1, fan2);
        } else {
            int val = atoi(argv[1]);
            if (!validate_fan_duty(val)) {
                printf("invalid fan duty %d! (valid range: %d-%d)\n", val,
                        MIN_FAN_DUTY, MAX_FAN_DUTY);
                return EXIT_FAILURE;
            }
            return main_test_fan(val);
        }
    }
    return EXIT_SUCCESS;
}

static int validate_fan_duty(int duty) {
    return duty >= MIN_FAN_DUTY && duty <= MAX_FAN_DUTY;
}

static void main_sanitize_cwd(void) {
    if (chdir("/") != 0 && chdir("/tmp") != 0)
        perror("chdir");
}

static int ec_load_ec_sys_module(void) {
    pid_t pid = fork();
    if (pid < 0)
        return EXIT_FAILURE;
    if (pid == 0) {
        if (chdir("/") != 0)
            _exit(126);
        execl("/sbin/modprobe", "modprobe", "ec_sys", (char*) NULL);
        execl("/usr/sbin/modprobe", "modprobe", "ec_sys", (char*) NULL);
        execl("/bin/modprobe", "modprobe", "ec_sys", (char*) NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return EXIT_FAILURE;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return EXIT_SUCCESS;
    return EXIT_FAILURE;
}

static void main_init_share(void) {
    void* shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED,
            -1, 0);
    if (shm == MAP_FAILED) {
        perror("mmap");
        exit(EXIT_FAILURE);
    }
    share_info = shm;
    share_info->exit = 0;
    share_info->cpu_temp = 0;
    share_info->gpu_temp = 0;
    share_info->fan1_duty = 0;
    share_info->fan2_duty = 0;
    share_info->fan1_rpms = 0;
    share_info->fan2_rpms = 0;
    share_info->auto_duty = 1;
    share_info->auto_duty_val_fan1 = 0;
    share_info->auto_duty_val_fan2 = 0;
    share_info->manual_next_fan1_duty = 0;
    share_info->manual_next_fan2_duty = 0;
    share_info->manual_prev_fan1_duty = 0;
    share_info->manual_prev_fan2_duty = 0;
}

static int main_ec_worker(void) {
    setuid(0);
    if (ec_load_ec_sys_module() != EXIT_SUCCESS)
        printf("warning: unable to load ec_sys kernel module\n");
    while (share_info->exit == 0) {
        // check parent
        if (parent_pid != 0 && kill(parent_pid, 0) == -1) {
            printf("worker on parent death\n");
            break;
        }
        // write EC (manual per-fan updates)
        int new_fan1_duty = share_info->manual_next_fan1_duty;
        if (new_fan1_duty != 0
                && new_fan1_duty != share_info->manual_prev_fan1_duty) {
            ec_write_fan1_duty(new_fan1_duty);
            share_info->manual_prev_fan1_duty = new_fan1_duty;
        }
        int new_fan2_duty = share_info->manual_next_fan2_duty;
        if (new_fan2_duty != 0
                && new_fan2_duty != share_info->manual_prev_fan2_duty) {
            ec_write_fan2_duty(new_fan2_duty);
            share_info->manual_prev_fan2_duty = new_fan2_duty;
        }
        // read EC
        int io_fd = open("/sys/kernel/debug/ec/ec0/io", O_RDONLY, 0);
        if (io_fd < 0) {
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        unsigned char buf[EC_REG_SIZE];
        ssize_t len = read(io_fd, buf, EC_REG_SIZE);
        switch (len) {
        case -1:
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            break;
        case 0x100:
            share_info->cpu_temp = buf[EC_REG_CPU_TEMP];
            share_info->gpu_temp = buf[EC_REG_GPU_TEMP];
            share_info->fan1_duty =
                    calculate_fan_duty(buf[EC_REG_FAN1_DUTY]);
            share_info->fan2_duty =
                    calculate_fan_duty(buf[EC_REG_FAN2_DUTY]);
            share_info->fan1_rpms = calculate_fan_rpms(
                    buf[EC_REG_FAN1_RPMS_HI], buf[EC_REG_FAN1_RPMS_LO]);
            share_info->fan2_rpms = calculate_fan_rpms(
                    buf[EC_REG_FAN2_RPMS_HI], buf[EC_REG_FAN2_RPMS_LO]);
            break;
        default:
            printf("wrong EC size from sysfs: %ld\n", len);
        }
        close(io_fd);
        // auto EC: CPU temp drives fan1, GPU temp drives fan2
        if (share_info->auto_duty == 1) {
            int next_fan1 = ec_auto_duty_adjust_for_temp(share_info->cpu_temp,
                    share_info->fan1_duty);
            if (next_fan1 != 0
                    && next_fan1 != share_info->auto_duty_val_fan1) {
                char s_time[256];
                get_time_string(s_time, 256, "%m/%d %H:%M:%S");
                printf("%s CPU=%d°C, auto CPU fan to %d%%\n", s_time,
                        share_info->cpu_temp, next_fan1);
                ec_write_fan1_duty(next_fan1);
                share_info->auto_duty_val_fan1 = next_fan1;
            }
            int next_fan2 = ec_auto_duty_adjust_for_temp(share_info->gpu_temp,
                    share_info->fan2_duty);
            if (next_fan2 != 0
                    && next_fan2 != share_info->auto_duty_val_fan2) {
                char s_time[256];
                get_time_string(s_time, 256, "%m/%d %H:%M:%S");
                printf("%s GPU=%d°C, auto GPU fan to %d%%\n", s_time,
                        share_info->gpu_temp, next_fan2);
                ec_write_fan2_duty(next_fan2);
                share_info->auto_duty_val_fan2 = next_fan2;
            }
        }
        //
        usleep(200 * 1000);
    }
    printf("worker quit\n");
    return EXIT_SUCCESS;
}

static void main_ui_worker(int argc, char** argv) {
    printf("Indicator...\n");
    int desktop_uid = getuid();
    setuid(desktop_uid);

    gtk_init(&argc, &argv);

    char icon_theme_path[PATH_MAX];
    char icon_file[PATH_MAX];
    if (ui_resolve_tray_icons(icon_theme_path, sizeof(icon_theme_path), icon_file,
                sizeof(icon_file)) != 0) {
        fprintf(stderr, "Tray icon missing (%s). Run: sudo make install\n",
                INDICATOR_ICON_FILE);
        exit(EXIT_FAILURE);
    }
    strncpy(ui_icon_source, icon_file, sizeof(ui_icon_source) - 1);
    ui_icon_source[sizeof(ui_icon_source) - 1] = '\0';
    ui_icon_published[0] = '\0';
    ui_icon_published_mtime = 0;
    if (ui_refresh_tray_icon(1) < 0) {
        fprintf(stderr, "Unable to publish tray icon from %s\n", ui_icon_source);
        exit(EXIT_FAILURE);
    }

    GtkWidget* indicator_menu = gtk_menu_new();
    GtkWidget* status_item = gtk_menu_item_new();
    status_menu_label = gtk_label_new("Loading...");
    gtk_label_set_xalign(GTK_LABEL(status_menu_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(status_menu_label), TRUE);
    gtk_widget_set_margin_start(status_menu_label, 12);
    gtk_widget_set_margin_end(status_menu_label, 12);
    gtk_widget_set_margin_top(status_menu_label, 6);
    gtk_widget_set_margin_bottom(status_menu_label, 6);
    gtk_container_add(GTK_CONTAINER(status_item), status_menu_label);
    gtk_widget_set_sensitive(status_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(indicator_menu), status_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(indicator_menu),
            gtk_separator_menu_item_new());

    for (int i = 0; i < menuitem_count; i++) {
        GtkWidget* item;
        if (strlen(menuitems[i].label) == 0) {
            item = gtk_separator_menu_item_new();
        } else {
            item = gtk_menu_item_new_with_label(menuitems[i].label);
            if (menuitems[i].callback != NULL)
                g_signal_connect_swapped(item, "activate",
                        menuitems[i].callback, (void*) menuitems[i].option);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(indicator_menu), item);
        menuitems[i].widget = item;
    }
    gtk_widget_show_all(indicator_menu);

    indicator = app_indicator_new_with_path(NAME, ui_icon_published,
            APP_INDICATOR_CATEGORY_HARDWARE, icon_theme_path);
    g_assert(indicator != NULL);
    app_indicator_set_icon_theme_path(indicator, icon_theme_path);
    app_indicator_set_label(indicator, "C-- G--", INDICATOR_LABEL_GUIDE);
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_ordering_index(indicator, -2);
    app_indicator_set_title(indicator, "Clevo Fan Control");
    app_indicator_set_menu(indicator, GTK_MENU(indicator_menu));

    g_timeout_add(500, ui_update, NULL);
    ui_toggle_menuitems(ui_active_type, ui_active_duty);
    gtk_main();
    printf("main on UI quit\n");
}

static void main_on_sigchld(int signum) {
    (void) signum;
    printf("main on worker quit signal\n");
    exit(EXIT_SUCCESS);
}

static void main_on_sigterm(int signum) {
    printf("main on signal: %s\n", strsignal(signum));
    if (share_info != NULL)
        share_info->exit = 1;
    exit(EXIT_SUCCESS);
}

static int main_dump_fan(void) {
    printf("Dump fan information\n");
    printf("  CPU Fan Duty: %d%%\n", ec_query_fan1_duty());
    printf("  CPU Fan RPMs: %d RPM\n", ec_query_fan1_rpms());
    printf("  GPU Fan Duty: %d%%\n", ec_query_fan2_duty());
    printf("  GPU Fan RPMs: %d RPM\n", ec_query_fan2_rpms());
    printf("  CPU Temp: %d°C\n", ec_query_cpu_temp());
    printf("  GPU Temp: %d°C\n", ec_query_gpu_temp());
    return EXIT_SUCCESS;
}

static int main_test_fan(int duty_percentage) {
    printf("Change both fans duty to %d%%\n", duty_percentage);
    if (ec_write_fan_duty(duty_percentage) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    printf("\n");
    main_dump_fan();
    return EXIT_SUCCESS;
}

static int main_test_fan_dual(int fan1_duty, int fan2_duty) {
    printf("Change CPU fan to %d%%, GPU fan to %d%%\n", fan1_duty, fan2_duty);
    if (ec_write_fan1_duty(fan1_duty) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (ec_write_fan2_duty(fan2_duty) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    printf("\n");
    main_dump_fan();
    return EXIT_SUCCESS;
}

static int ui_build_icon_path(const char* theme_dir, char* icon_file,
        size_t icon_size) {
    static const char subpath[] = "/hicolor/scalable/status/";
    size_t need = strlen(theme_dir) + sizeof(subpath) - 1
            + strlen(INDICATOR_ICON_FILE) + 1;
    if (need > icon_size)
        return -1;
    snprintf(icon_file, icon_size, "%s%s%s", theme_dir, subpath,
            INDICATOR_ICON_FILE);
    return 0;
}

static int ui_resolve_tray_icons(char* theme_path, size_t theme_size,
        char* icon_file, size_t icon_size) {
    const char* install_theme = INDICATOR_ICON_INSTALL_DIR;
    char install_icon[PATH_MAX];
    char dev_theme[PATH_MAX];
    char dev_icon[PATH_MAX];
    int has_install = 0;
    int has_dev = 0;

    if (ui_build_icon_path(install_theme, install_icon, sizeof(install_icon)) == 0
            && access(install_icon, R_OK) == 0)
        has_install = 1;

    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len > 0) {
        exe[len] = '\0';
        char exe_copy[PATH_MAX];
        strncpy(exe_copy, exe, sizeof(exe_copy) - 1);
        exe_copy[sizeof(exe_copy) - 1] = '\0';
        char* bindir = dirname(exe_copy);
        char icons_path[PATH_MAX];
        snprintf(icons_path, sizeof(icons_path), "%s/../icons", bindir);
        if (realpath(icons_path, dev_theme) != NULL
                && ui_build_icon_path(dev_theme, dev_icon, sizeof(dev_icon)) == 0
                && access(dev_icon, R_OK) == 0)
            has_dev = 1;
    }

    const char* base = NULL;
    if (has_install && has_dev) {
        struct stat st_install;
        struct stat st_dev;
        if (stat(install_icon, &st_install) == 0 && stat(dev_icon, &st_dev) == 0
                && st_dev.st_mtime > st_install.st_mtime)
            base = dev_theme;
        else
            base = install_theme;
    } else if (has_dev) {
        base = dev_theme;
    } else if (has_install) {
        base = install_theme;
    } else {
        return -1;
    }

    if (ui_build_icon_path(base, icon_file, icon_size) != 0)
        return -1;
    strncpy(theme_path, base, theme_size);
    theme_path[theme_size - 1] = '\0';
    return 0;
}

/* GNOME caches tray images by path; publish a runtime copy keyed by mtime. */
static int ui_refresh_tray_icon(int force) {
    struct stat st;
    if (stat(ui_icon_source, &st) != 0)
        return -1;
    if (!force && st.st_mtime == ui_icon_published_mtime
            && ui_icon_published[0] != '\0'
            && access(ui_icon_published, R_OK) == 0)
        return 0;

    const char* runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime == NULL || runtime[0] == '\0')
        runtime = "/tmp";

    char dir[PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/clevo-indicator", runtime);
    if (n < 0 || (size_t) n >= sizeof(dir))
        return -1;
    if (g_mkdir_with_parents(dir, 0700) != 0)
        return -1;

    char published[PATH_MAX];
    n = snprintf(published, sizeof(published), "%s/fan-%lld.jpg", dir,
            (long long) st.st_mtime);
    if (n < 0 || (size_t) n >= sizeof(published))
        return -1;

    GFile* src = g_file_new_for_path(ui_icon_source);
    GFile* dst = g_file_new_for_path(published);
    GError* error = NULL;
    if (!g_file_copy(src, dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error)) {
        g_object_unref(src);
        g_object_unref(dst);
        if (error != NULL) {
            fprintf(stderr, "Tray icon copy failed: %s\n", error->message);
            g_error_free(error);
        }
        return -1;
    }
    g_object_unref(src);
    g_object_unref(dst);

    strncpy(ui_icon_published, published, sizeof(ui_icon_published) - 1);
    ui_icon_published[sizeof(ui_icon_published) - 1] = '\0';
    ui_icon_published_mtime = st.st_mtime;
    return 1;
}

static void ui_format_status_menu(char* buf, size_t size) {
    snprintf(buf, size,
            "CPU %d°C   GPU %d°C\n"
            "CPU Fan: %d%%  (%d RPM)\n"
            "GPU Fan: %d%%  (%d RPM)",
            share_info->cpu_temp, share_info->gpu_temp, share_info->fan1_duty,
            share_info->fan1_rpms, share_info->fan2_duty, share_info->fan2_rpms);
}

static gboolean ui_update(gpointer user_data) {
    (void) user_data;
    if (share_info == NULL || indicator == NULL)
        return G_SOURCE_CONTINUE;
    char tray_label[32];
    char menu_label[512];
    snprintf(tray_label, sizeof(tray_label), "C%d G%d", share_info->cpu_temp,
            share_info->gpu_temp);
    ui_format_status_menu(menu_label, sizeof(menu_label));
    app_indicator_set_label(indicator, tray_label, INDICATOR_LABEL_GUIDE);
    if (ui_refresh_tray_icon(0) > 0)
        app_indicator_set_icon(indicator, ui_icon_published);
    if (status_menu_label != NULL)
        gtk_label_set_text(GTK_LABEL(status_menu_label), menu_label);
    return G_SOURCE_CONTINUE;
}

static void ui_clear_manual_requests(void) {
    share_info->manual_next_fan1_duty = 0;
    share_info->manual_next_fan2_duty = 0;
    share_info->manual_prev_fan1_duty = 0;
    share_info->manual_prev_fan2_duty = 0;
}

static void ui_command_set_fan(long fan_duty) {
    int fan_duty_val = (int) fan_duty;
    if (fan_duty_val == 0) {
        printf("clicked on both fans auto\n");
        share_info->auto_duty = 1;
        share_info->auto_duty_val_fan1 = 0;
        share_info->auto_duty_val_fan2 = 0;
        ui_clear_manual_requests();
        ui_active_type = AUTO;
        ui_active_duty = 0;
    } else {
        printf("clicked on both fans duty: %d\n", fan_duty_val);
        share_info->auto_duty = 0;
        share_info->auto_duty_val_fan1 = 0;
        share_info->auto_duty_val_fan2 = 0;
        /* reset prev so the worker always rewrites, even if duty equals
         * a previously requested value while auto control changed it. */
        share_info->manual_prev_fan1_duty = 0;
        share_info->manual_prev_fan2_duty = 0;
        share_info->manual_next_fan1_duty = fan_duty_val;
        share_info->manual_next_fan2_duty = fan_duty_val;
        ui_active_type = MANUAL_LINKED;
        ui_active_duty = fan_duty_val;
    }
    ui_toggle_menuitems(ui_active_type, ui_active_duty);
}

static void ui_command_set_fan1(long fan_duty) {
    int fan_duty_val = (int) fan_duty;
    printf("clicked on CPU fan duty: %d\n", fan_duty_val);
    share_info->auto_duty = 0;
    share_info->auto_duty_val_fan1 = 0;
    share_info->manual_prev_fan1_duty = 0;
    share_info->manual_next_fan1_duty = fan_duty_val;
    ui_active_type = MANUAL_FAN1;
    ui_active_duty = fan_duty_val;
    ui_toggle_menuitems(ui_active_type, ui_active_duty);
}

static void ui_command_set_fan2(long fan_duty) {
    int fan_duty_val = (int) fan_duty;
    printf("clicked on GPU fan duty: %d\n", fan_duty_val);
    share_info->auto_duty = 0;
    share_info->auto_duty_val_fan2 = 0;
    share_info->manual_prev_fan2_duty = 0;
    share_info->manual_next_fan2_duty = fan_duty_val;
    ui_active_type = MANUAL_FAN2;
    ui_active_duty = fan_duty_val;
    ui_toggle_menuitems(ui_active_type, ui_active_duty);
}

static void ui_command_quit(gchar* command) {
    (void) command;
    printf("clicked on quit\n");
    gtk_main_quit();
}

static void ui_toggle_menuitems(MenuItemType active_type, int active_duty) {
    for (int i = 0; i < menuitem_count; i++) {
        if (menuitems[i].widget == NULL)
            continue;
        if (menuitems[i].type == NA)
            continue;
        if (active_type == AUTO)
            gtk_widget_set_sensitive(menuitems[i].widget,
                    menuitems[i].type != AUTO);
        else
            gtk_widget_set_sensitive(menuitems[i].widget,
                    menuitems[i].type != active_type
                            || (int) menuitems[i].option != active_duty);
    }
}

static int ec_init(void) {
    if (ioperm(EC_DATA, 1, 1) != 0) {
        if (geteuid() != 0)
            fprintf(stderr,
                    "Hint: need setuid root on the binary (sudo make install; "
                    "ls -l should show 'rws' on /usr/local/bin/clevo-indicator)\n");
        return EXIT_FAILURE;
    }
    if (ioperm(EC_SC, 1, 1) != 0)
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

static void ec_on_sigterm(int signum) {
    printf("ec on signal: %s\n", strsignal(signum));
    if (share_info != NULL)
        share_info->exit = 1;
}

static int ec_auto_duty_adjust_for_temp(int temp, int duty) {
    if (temp >= 80 && duty < 100)
        return 100;
    if (temp >= 70 && duty < 90)
        return 90;
    if (temp >= 60 && duty < 80)
        return 80;
    if (temp >= 50 && duty < 70)
        return 70;
    if (temp >= 40 && duty < 60)
        return 60;
    if (temp >= 30 && duty < 50)
        return 50;
    if (temp >= 20 && duty < 40)
        return 40;
    if (temp >= 10 && duty < 30)
        return 30;
    if (temp <= 15 && duty > 30)
        return 30;
    if (temp <= 25 && duty > 40)
        return 40;
    if (temp <= 35 && duty > 50)
        return 50;
    if (temp <= 45 && duty > 60)
        return 60;
    if (temp <= 55 && duty > 70)
        return 70;
    if (temp <= 65 && duty > 80)
        return 80;
    if (temp <= 75 && duty > 90)
        return 90;
    return 0;
}

static int ec_query_cpu_temp(void) {
    return ec_io_read(EC_REG_CPU_TEMP);
}

static int ec_query_gpu_temp(void) {
    return ec_io_read(EC_REG_GPU_TEMP);
}

static int ec_query_fan1_duty(void) {
    int raw_duty = ec_io_read(EC_REG_FAN1_DUTY);
    return calculate_fan_duty(raw_duty);
}

static int ec_query_fan2_duty(void) {
    int raw_duty = ec_io_read(EC_REG_FAN2_DUTY);
    return calculate_fan_duty(raw_duty);
}

static int ec_query_fan1_rpms(void) {
    int raw_rpm_hi = ec_io_read(EC_REG_FAN1_RPMS_HI);
    int raw_rpm_lo = ec_io_read(EC_REG_FAN1_RPMS_LO);
    return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
}

static int ec_query_fan2_rpms(void) {
    int raw_rpm_hi = ec_io_read(EC_REG_FAN2_RPMS_HI);
    int raw_rpm_lo = ec_io_read(EC_REG_FAN2_RPMS_LO);
    return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
}

static int ec_duty_to_raw(int duty_percentage) {
    return (int) (((double) duty_percentage) / 100.0 * 255.0);
}

static int ec_write_fan1_duty(int duty_percentage) {
    if (!validate_fan_duty(duty_percentage)) {
        printf("Wrong CPU fan duty to write: %d (valid range: %d-%d)\n",
                duty_percentage, MIN_FAN_DUTY, MAX_FAN_DUTY);
        return EXIT_FAILURE;
    }
    return ec_io_do(EC_FAN_CMD, EC_FAN_PORT_CPU,
            (uint8_t) ec_duty_to_raw(duty_percentage));
}

static int ec_write_fan2_duty(int duty_percentage) {
    if (!validate_fan_duty(duty_percentage)) {
        printf("Wrong GPU fan duty to write: %d (valid range: %d-%d)\n",
                duty_percentage, MIN_FAN_DUTY, MAX_FAN_DUTY);
        return EXIT_FAILURE;
    }
    return ec_io_do(EC_FAN_CMD, EC_FAN_PORT_GPU,
            (uint8_t) ec_duty_to_raw(duty_percentage));
}

static int ec_write_fan_duty(int duty_percentage) {
    if (ec_write_fan1_duty(duty_percentage) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    return ec_write_fan2_duty(duty_percentage);
}

static int ec_io_wait(const uint32_t port, const uint32_t flag,
        const char value) {
    uint8_t data = inb(port);
    int i = 0;
    while ((((data >> flag) & 0x1) != value) && (i++ < 100)) {
        usleep(1000);
        data = inb(port);
    }
    if (i >= 100) {
        printf("wait_ec error on port 0x%x, data=0x%x, flag=0x%x, value=0x%x\n",
                port, data, flag, value);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static uint8_t ec_io_read(const uint32_t port) {
    ec_io_wait(EC_SC, IBF, 0);
    outb(EC_SC_READ_CMD, EC_SC);

    ec_io_wait(EC_SC, IBF, 0);
    outb(port, EC_DATA);

    ec_io_wait(EC_SC, OBF, 1);
    uint8_t value = inb(EC_DATA);

    return value;
}

static int ec_io_do(const uint32_t cmd, const uint32_t port,
        const uint8_t value) {
    ec_io_wait(EC_SC, IBF, 0);
    outb(cmd, EC_SC);

    ec_io_wait(EC_SC, IBF, 0);
    outb(port, EC_DATA);

    ec_io_wait(EC_SC, IBF, 0);
    outb(value, EC_DATA);

    return ec_io_wait(EC_SC, IBF, 0);
}

static int calculate_fan_duty(int raw_duty) {
    return (int) ((double) raw_duty / 255.0 * 100.0);
}

static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low) {
    int raw_rpm = (raw_rpm_high << 8) + raw_rpm_low;
    return raw_rpm > 0 ? (2156220 / raw_rpm) : 0;
}

static int check_proc_instances(const char* proc_name) {
    int proc_name_len = strlen(proc_name);
    pid_t this_pid = getpid();
    DIR* dir;
    if (!(dir = opendir("/proc"))) {
        perror("can't open /proc");
        return -1;
    }
    int instance_count = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        char* endptr;
        long lpid = strtol(ent->d_name, &endptr, 10);
        if (*endptr != '\0')
            continue;
        if (lpid == this_pid)
            continue;
        char buf[512];
        snprintf(buf, sizeof(buf), "/proc/%ld/comm", lpid);
        FILE* fp = fopen(buf, "r");
        if (fp) {
            if (fgets(buf, sizeof(buf), fp) != NULL) {
                if ((buf[proc_name_len] == '\n' || buf[proc_name_len] == '\0')
                        && strncmp(buf, proc_name, proc_name_len) == 0) {
                    fprintf(stderr, "Process: %ld\n", lpid);
                    instance_count += 1;
                }
            }
            fclose(fp);
        }
    }
    closedir(dir);
    return instance_count;
}

static void get_time_string(char* buffer, size_t max, const char* format) {
    time_t timer;
    struct tm tm_info;
    time(&timer);
    localtime_r(&timer, &tm_info);
    strftime(buffer, max, format, &tm_info);
}

static void signal_term(__sighandler_t handler) {
    signal(SIGHUP, handler);
    signal(SIGINT, handler);
    signal(SIGQUIT, handler);
    signal(SIGPIPE, handler);
    signal(SIGALRM, handler);
    signal(SIGTERM, handler);
    signal(SIGUSR1, handler);
    signal(SIGUSR2, handler);
}
