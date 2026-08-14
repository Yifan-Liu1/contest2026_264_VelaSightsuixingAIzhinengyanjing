/****************************************************************************
 * BK7258 peripheral self-test.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <net/if.h>

#include <nuttx/clock.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/ioctl.h>
#include <arch/chip/bk7258_psram.h>

static unsigned int test_proc_file(const char *path)
{
  char buffer[128];
  int fd;
  ssize_t n;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      printf("FAIL %s open errno=%d\n", path, errno);
      return 1;
    }

  n = read(fd, buffer, sizeof(buffer));
  close(fd);
  if (n <= 0)
    {
      printf("FAIL %s read=%ld\n", path, (long)n);
      return 1;
    }

  printf("PASS %s bytes=%ld\n", path, (long)n);
  return 0;
}

static unsigned int test_psram(void)
{
  static const unsigned char seed = 0xa5;
  unsigned char buffer[256];
  struct bk7258_psram_info info;
  unsigned int i;
  void *memory;

  if (!bk7258_psram_is_online() || bk7258_psram_info(&info) < 0 ||
      info.base != 0x60000000u || info.capacity != 0x01000000u)
    {
      printf("FAIL psram online/info\n");
      return 1;
    }

  memory = bk7258_psram_memalign(32, sizeof(buffer));
  if (memory == NULL || !bk7258_psram_contains(memory, sizeof(buffer)))
    {
      printf("FAIL psram allocation\n");
      if (memory != NULL)
        {
          bk7258_psram_free(memory);
        }
      return 1;
    }

  for (i = 0; i < sizeof(buffer); i++)
    {
      buffer[i] = (unsigned char)(seed ^ i);
    }
  memcpy(memory, buffer, sizeof(buffer));
  memset(buffer, 0, sizeof(buffer));
  memcpy(buffer, memory, sizeof(buffer));
  bk7258_psram_free(memory);

  for (i = 0; i < sizeof(buffer); i++)
    {
      if (buffer[i] != (unsigned char)(seed ^ i))
        {
          printf("FAIL psram read/write index=%u\n", i);
          return 1;
        }
    }

  printf("PASS psram base=0x%08lx size=%lu\n",
         (unsigned long)info.base, (unsigned long)info.capacity);
  return 0;
}

static unsigned int test_uart(void)
{
  static const char message[] = "periph_selftest: mailbox UART TX\n";
  int fd;
  ssize_t written;

  fd = open("/dev/ttyMB0", O_WRONLY);
  if (fd < 0)
    {
      printf("FAIL /dev/ttyMB0 open errno=%d\n", errno);
      return 1;
    }

  written = write(fd, message, sizeof(message) - 1);
  if (written == (ssize_t)(sizeof(message) - 1))
    {
      (void)tcdrain(fd);
    }
  close(fd);
  if (written != (ssize_t)(sizeof(message) - 1))
    {
      printf("FAIL /dev/ttyMB0 write=%ld\n", (long)written);
      return 1;
    }

  printf("PASS mailbox UART TX\n");
  return 0;
}

static unsigned int test_storage(void)
{
  uint8_t sector[512];
  struct geometry geo;
  struct inode *inode;
  DIR *dir;
  struct dirent *entry;
  const char *mountdev = "/dev/mmcsd0p0";
  int ret;
  ssize_t nsectors;
  bool mounted = false;
  char first_name[NAME_MAX + 1];

  ret = open_blockdriver("/dev/mmcsd0", MS_RDONLY, &inode);
  if (ret < 0)
    {
      printf("FAIL storage open /dev/mmcsd0 error=%d\n", ret);
      return 1;
    }

  ret = inode->u.i_bops->geometry(inode, &geo);
  if (ret < 0 || geo.geo_sectorsize != sizeof(sector) ||
      geo.geo_nsectors == 0)
    {
      printf("FAIL storage geometry ret=%d sector=%u count=%lu\n", ret,
             (unsigned)geo.geo_sectorsize,
             (unsigned long)geo.geo_nsectors);
      close_blockdriver(inode);
      return 1;
    }

  nsectors = inode->u.i_bops->read(inode, sector, 0, 1);
  close_blockdriver(inode);
  if (nsectors != 1)
    {
      printf("FAIL storage sector0 read=%ld\n", (long)nsectors);
      return 1;
    }

  if (sector[510] != 0x55 || sector[511] != 0xaa)
    {
      printf("FAIL storage sector0 signature=%02x%02x\n",
             sector[510], sector[511]);
      return 1;
    }

  printf("PASS storage geometry sector=%u count=%lu signature=55aa\n",
         (unsigned)geo.geo_sectorsize, (unsigned long)geo.geo_nsectors);

  (void)mkdir("/mnt", 0777);
  (void)mkdir("/mnt/sd", 0777);
  ret = nx_mount(mountdev, "/mnt/sd", "vfat", MS_RDONLY, NULL);
  if (ret < 0)
    {
      mountdev = "/dev/mmcsd0";
      ret = nx_mount(mountdev, "/mnt/sd", "vfat", MS_RDONLY, NULL);
    }

  if (ret < 0)
    {
      printf("FAIL storage vfat mount dev=%s error=%d\n", mountdev, ret);
      return 1;
    }

  mounted = true;
  dir = opendir("/mnt/sd");
  if (dir == NULL)
    {
      printf("FAIL storage directory errno=%d\n", errno);
      (void)nx_umount2("/mnt/sd", 0);
      return 1;
    }

  entry = readdir(dir);
  if (entry != NULL)
    {
      strlcpy(first_name, entry->d_name, sizeof(first_name));
    }
  else
    {
      first_name[0] = '\0';
    }
  closedir(dir);
  ret = entry == NULL ? -EIO : 0;
  printf("%s storage vfat mount dev=%s first=%s\n",
         ret == 0 ? "PASS" : "FAIL", mountdev,
         first_name[0] == '\0' ? "<empty>" : first_name);
  if (mounted)
    {
      (void)nx_umount2("/mnt/sd", 0);
    }

  return ret == 0 ? 0 : 1;
}

int periph_selftest_main(int argc, char *argv[])
{
  clock_t before;
  clock_t after;
  unsigned int failures = 0;

  (void)argc;
  (void)argv;
  printf("SELFTEST BEGIN\n");

  if (if_nametoindex("wlan0") == 0)
    {
      printf("FAIL wlan0 interface missing\n");
      failures++;
    }
  else
    {
      printf("PASS wlan0 interface present\n");
    }

  failures += test_uart();
  failures += test_psram();
  failures += test_storage();
  failures += test_proc_file("/proc/version");
  failures += test_proc_file("/proc/uptime");
  failures += test_proc_file("/proc/meminfo");
  before = clock_systime_ticks();
  usleep(10000);
  after = clock_systime_ticks();
  if (after == before)
    {
      printf("FAIL systick did not advance\n");
      failures++;
    }
  else
    {
      printf("PASS systick\n");
    }

  printf("Wi-Fi separate test: ifup wlan0; wapi scan wlan0; ifconfig wlan0\n");
  printf("SELFTEST %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
  return failures == 0 ? 0 : 1;
}
