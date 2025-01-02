#include <atomic>
#include <cstring>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <mutex>
#include <printf.h>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <sys/socket.h>


uint64_t g_client_count = 0;        // default to g_device_count
uint64_t g_device_count = 10;
uint64_t g_metric_count = 10;
uint64_t g_sensor_count = 10;

int g_interval_ms = -1;             // interval between each patch of metrics; -1 => same as g_step_ms
uint64_t g_step_ms = 30000;         // interval between samplings
uint64_t g_start_timestamp = 0;     // timestamp of 1st sampling
uint64_t g_end_timestamp = 0;       // timestamp of last sampling
uint64_t g_duration_ms = 0;

bool g_run_as_daemon = false;

std::string g_log_filename;
std::string g_ticktock_db("127.0.0.1:6180");
int g_ticktock_port = 6180;

thread_local std::string g_thread_id;
thread_local int g_target_fd = -1;
std::thread *g_client_threads;
FILE *g_log_file = nullptr;

// statistics
std::atomic<uint64_t> g_total_dps_cnt{0};


void
process_opts(const char *name, const char *value)
{
    if (std::strcmp(name, "client") == 0)
        g_client_count = std::stoll(value);
    else if (std::strcmp(name, "device") == 0)
        g_device_count = std::stoll(value);
    else if (std::strcmp(name, "duration") == 0)
        g_duration_ms = std::stoll(value);
    else if (std::strcmp(name, "end") == 0)
        g_end_timestamp = std::stoll(value);
    else if (std::strcmp(name, "metric") == 0)
        g_metric_count = std::stoll(value);
    else if (std::strcmp(name, "sensor") == 0)
        g_sensor_count = std::stoll(value);
    else if (std::strcmp(name, "interval") == 0)
        g_interval_ms = std::stoll(value);
    else if (std::strcmp(name, "log") == 0)
        g_log_filename = value;
    else if (std::strcmp(name, "start") == 0)
        g_start_timestamp = std::stoll(value);
    else if (std::strcmp(name, "step") == 0)
        g_step_ms = std::stoll(value);
    else if (std::strcmp(name, "ticktock") == 0)
        g_ticktock_db = value;
    else
        throw std::runtime_error(std::string("Unrecognized cmdline option: ") + name);
}

static int
process_cmdline_opts(int argc, char *argv[])
{
    int c;
    int digit_optind = 0;
    const char *optstring = "d";
    static struct option long_options[] =
    {
        { "client",         required_argument,  0,  0 },
        { "device",         required_argument,  0,  0 },
        { "duration",       required_argument,  0,  0 },
        { "end",            required_argument,  0,  0 },
        { "interval",       required_argument,  0,  0 },    // interval for metric sampling
        { "log",            required_argument,  0,  0 },    // log file
        { "metric",         required_argument,  0,  0 },
        { "progress",       required_argument,  0,  0 },    // interval for progress report
        { "sensor",         required_argument,  0,  0 },
        { "start",          required_argument,  0,  0 },
        { "step",           required_argument,  0,  0 },
        { "ticktock",       required_argument,  0,  0 },    // IP of TickTockDB, optionally port number
        {0, 0, 0, 0},
    };

    while (1)
    {
        int this_option_optind = optind ? optind : 1;
        int option_index = 0;

        c = getopt_long(argc, argv, optstring, long_options, &option_index);
        if (c == -1) break;

        switch (c)
        {
            case 0:
                process_opts(long_options[option_index].name, optarg);
                break;

            case 'd':
                g_run_as_daemon = true;     // run in daemon mode
                break;

            case '?':
                fprintf(stderr, "Unknown option: '\\x%x'.\n", optopt);
                return 1;
            default:
                return 2;
        }
    }

    if (optind < argc)
    {
        fprintf(stderr, "Unknown options that are ignored: ");
        while (optind < argc) fprintf(stderr, "%s ", argv[optind++]);
        fprintf(stderr, "\n");
    }

    return 0;
}

static void
daemonize()
{
    if (daemon(1, 0) != 0)
        fprintf(stderr, "daemon() failed: errno = %d\n", errno);
}

void
ts_now(time_t& sec, unsigned int& msec)
{
    using namespace std::chrono;
    system_clock::time_point now = system_clock::now();
    sec = system_clock::to_time_t(now);

    milliseconds ms = duration_cast<milliseconds>(now.time_since_epoch());
    msec = ms.count() % 1000;
}   

time_t
ts_now()
{
    time_t sec;
    unsigned int msec;
    ts_now(sec, msec);
    return sec * 1000 + msec;
}

// one time initialization, at the beginning of the execution
static void
initialize()
{
    g_thread_id = "main";
    std::srand(std::time(0));

    if (g_run_as_daemon)
    {
        // get our working directory
        daemonize();
    }

    // verify options
    if (g_start_timestamp == 0)
        g_start_timestamp = ts_now();

    if (g_end_timestamp == 0)
    {
        if (g_duration_ms == 0)
            g_duration_ms = 3600000;    // default to 1 hour
        g_end_timestamp = g_start_timestamp + g_duration_ms;
    }

    if (g_end_timestamp <= g_start_timestamp)
        throw std::runtime_error(std::string("start-time ") + std::to_string(g_start_timestamp) +
                                 std::string(" should be earlier than end-time ") + std::to_string(g_end_timestamp));

    if (g_step_ms <= 0)
        throw std::runtime_error(std::string("step should be greater than 0: ") + std::to_string(g_step_ms));

    if (g_client_count == 0)
        g_client_count = g_device_count;

    if (g_device_count < g_client_count)
        throw std::runtime_error(std::string("number of clients cannot be greater than number of devices"));

    if (! g_log_filename.empty())
        g_log_file = fopen(g_log_filename.c_str(), "a+");

    auto pos = g_ticktock_db.find_first_of(':');

    if (pos != std::string::npos)
    {
        std::string host = g_ticktock_db.substr(0, pos);
        std::string port = g_ticktock_db.substr(pos+1);

        g_ticktock_db = host;
        g_ticktock_port = std::stoi(port);
    }

    if (g_interval_ms < 0)
        g_interval_ms = g_step_ms;

    printf("Running against TickTockDB at %s:%d\n", g_ticktock_db.c_str(), g_ticktock_port);
    printf("start=%lu, end=%lu, duration=%lu, step=%lu, interval=%d\n",
        g_start_timestamp, g_end_timestamp, g_duration_ms, g_step_ms, g_interval_ms);
}

void
log(const char *format, ...)
{
    size_t len = std::strlen(format);
    char fmt[len + 128];

    time_t sec;
    unsigned int msec;

    ts_now(sec, msec);

    struct tm timeinfo;
    localtime_r(&sec, &timeinfo);
    std::strftime(fmt, sizeof(fmt), "%Y-%m-%d %H:%M:%S", &timeinfo);
    sprintf(fmt+19, ".%03d [%s] %s", msec, g_thread_id.c_str(), format);

    if (format[len-1] != '\n')
        std::strcat(fmt, "\n");

    va_list args;
    va_start(args, format);

    static std::mutex s_mutex;
    std::lock_guard<std::mutex> guard(s_mutex);

    std::vprintf(fmt, args);

    if (g_log_file != nullptr)
    {
        va_list args;
        va_start(args, format);
        std::vfprintf(g_log_file, fmt, args);
    }
}

void
connect()
{
    struct sockaddr_in addr;

    g_target_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (g_target_fd >= 0)
    {
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(g_ticktock_port);
        inet_pton(AF_INET, g_ticktock_db.c_str(), &addr.sin_addr.s_addr);

        if (::connect(g_target_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
            printf("Failed to connect to %s at port %d, errno=%d",
                g_ticktock_db.c_str(), g_ticktock_port, errno);
    }
}

void
disconnect()
{
    if (g_target_fd >= 0)
    {
        close(g_target_fd);
        g_target_fd = -1;
    }
}

void
send_to_target(const char *buff, int len)
{
    int sent_total = 0;

    while (len > 0)
    {
        int sent = send(g_target_fd, buff+sent_total, len, 0);
        if (sent == -1) break;
        len -= sent;
        sent_total += sent;
    }
}

void
client_main(unsigned int id)
{
    g_thread_id += "client_";
    g_thread_id += std::to_string(id);

    connect();

    double duration = g_end_timestamp - g_start_timestamp;
    uint64_t interval = (g_interval_ms >= 0) ? g_interval_ms : g_step_ms;

    uint64_t device_start = id * (g_device_count/g_client_count);
    uint64_t device_end = (id+1) * (g_device_count/g_client_count);
    uint64_t i = 0;

    for (uint64_t ts = g_start_timestamp; ts <= g_end_timestamp; ts += g_step_ms)
    {
        double percent = 100.0 * (double)(ts - g_start_timestamp) / duration;

        for (unsigned int d = device_start; d < device_end; d++)
        {
            for (unsigned int m = 0; m < g_metric_count; m++)
            {
                char buff[8192];
                int n = 0;
                double v = 1.2;

                n = sprintf(buff, "metric_%u,device=d_%u ", m, d);

                for (unsigned int s = 0; s < g_sensor_count; s++)
                    n += sprintf(buff+n, "s_%u=%f,", s, v);

                buff[n-1] = '\n';
                buff[n] = 0;

                send_to_target(buff, n);
                g_total_dps_cnt.fetch_add(g_sensor_count, std::memory_order_relaxed);
            }
        }

        if ((++i % 10000) == 0)
            log("%.2f percent done", percent);

        if ((g_interval_ms > 0) && ((ts + g_step_ms) <= g_end_timestamp))
            std::this_thread::sleep_for(std::chrono::milliseconds(g_interval_ms));
    }
}

int
main(int argc, char *argv[])
{
    if (process_cmdline_opts(argc, argv) != 0)
        return 1;

    try
    {
        initialize();
    }
    catch (std::exception &ex)
    {
        fprintf(stderr, "Initialization failed: %s\n", ex.what());
        return 9;
    }
    catch (...)
    {
        fprintf(stderr, "Initialization failed. Abort!\n");
        return 9;
    }

    g_client_threads = new std::thread[g_client_count];
    time_t begin = ts_now();

    for (unsigned int i = 0; i < g_client_count; i++)
        g_client_threads[i] = std::thread(&client_main, i);

    // wait for clients to finish
    for (unsigned int i = 0; i < g_client_count; i++)
    {
        if (g_client_threads[i].joinable())
            g_client_threads[i].join();
    }

    time_t end = ts_now();

    // cleanup
    if (g_log_file != nullptr)
        fclose(g_log_file);
    disconnect();

    // print summary
    std::locale loc("");
    std::cerr.imbue(loc);
    std::cerr << "Grand Total  = " << g_total_dps_cnt.load() << " dps" << std::endl;
    std::cerr << "Elapsed Time = " << (end - begin) << " ms" << std::endl;
    printf("Throughput   = %.2f dps/sec\n", 1000.0*(double)g_total_dps_cnt.load()/(double)(end-begin));
    //std::cerr << "Throughput   = " << 1000.0*(double)g_total_dps_cnt.load()/(double)(end-begin) << "dps/sec" << std::endl;

    return 0;
}
