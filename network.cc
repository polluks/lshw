#include "network.h"
#include "osutils.h"
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if_arp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/types.h>
using namespace std;

#ifndef SIOCETHTOOL
#define SIOCETHTOOL     0x8946
#endif
typedef unsigned long long u64;
typedef __uint32_t u32;
typedef __uint16_t u16;
typedef __uint8_t u8;

struct ethtool_cmd
{
  u32 cmd;
  u32 supported;		/* Features this interface supports */
  u32 advertising;		/* Features this interface advertises */
  u16 speed;			/* The forced speed, 10Mb, 100Mb, gigabit */
  u8 duplex;			/* Duplex, half or full */
  u8 port;			/* Which connector port */
  u8 phy_address;
  u8 transceiver;		/* Which tranceiver to use */
  u8 autoneg;			/* Enable or disable autonegotiation */
  u32 maxtxpkt;			/* Tx pkts before generating tx int */
  u32 maxrxpkt;			/* Rx pkts before generating rx int */
  u32 reserved[4];
};

#define ETHTOOL_BUSINFO_LEN     32
/* these strings are set to whatever the driver author decides... */
struct ethtool_drvinfo
{
  u32 cmd;
  char driver[32];		/* driver short name, "tulip", "eepro100" */
  char version[32];		/* driver version string */
  char fw_version[32];		/* firmware version string, if applicable */
  char bus_info[ETHTOOL_BUSINFO_LEN];	/* Bus info for this IF. */
  /*
   * For PCI devices, use pci_dev->slot_name. 
   */
  char reserved1[32];
  char reserved2[16];
  u32 n_stats;			/* number of u64's from ETHTOOL_GSTATS */
  u32 testinfo_len;
  u32 eedump_len;		/* Size of data from ETHTOOL_GEEPROM (bytes) */
  u32 regdump_len;		/* Size of data from ETHTOOL_GREGS (bytes) */
};

/* CMDs currently supported */
#define ETHTOOL_GSET            0x00000001	/* Get settings. */
#define ETHTOOL_GDRVINFO        0x00000003	/* Get driver info. */

bool load_interfaces(vector < string > &interfaces)
{
  vector < string > procnetdev;

  interfaces.clear();
  if (!loadfile("/proc/net/dev", procnetdev))
    return false;

  if (procnetdev.size() <= 2)
    return false;

  // get rid of header (2 lines)
  procnetdev.erase(procnetdev.begin());
  procnetdev.erase(procnetdev.begin());

  for (unsigned int i = 0; i < procnetdev.size(); i++)
  {
    // extract interfaces names
    size_t pos = procnetdev[i].find(':');

    if (pos != string::npos)
      interfaces.push_back(hw::strip(procnetdev[i].substr(0, pos)));
  }

  return true;
}

static string getmac(const unsigned char *mac)
{
  char buff[5];
  string result = "";
  bool valid = false;

  for (int i = 0; i < 6; i++)
  {
    snprintf(buff, sizeof(buff), "%02x", mac[i]);

    valid |= (mac[i] != 0);

    if (i == 0)
      result = string(buff);
    else
      result += ":" + string(buff);
  }

  if (valid)
    return result;
  else
    return "";
}

static const char *hwname(int t)
{
  switch (t)
  {
  case ARPHRD_ETHER:
    return "Ethernet";
  case ARPHRD_SLIP:
    return "SLIP";
  case ARPHRD_LOOPBACK:
    return "loopback";
  case ARPHRD_FDDI:
    return "FDDI";
  case ARPHRD_IRDA:
    return "IRDA";
  case ARPHRD_PPP:
    return "PPP";
  case ARPHRD_X25:
    return "X25";
  case ARPHRD_TUNNEL:
    return "IPtunnel";
  case ARPHRD_DLCI:
    return "Framerelay.DLCI";
  case ARPHRD_FRAD:
    return "Framerelay.AD";
  default:
    return "";
  }
}

static string print_ip(struct sockaddr_in *in)
{
  return string(inet_ntoa(in->sin_addr));
}

static void scan_ip(hwNode & interface)
{
  int fd = socket(AF_INET, SOCK_DGRAM, 0);

  if (fd > 0)
  {
    struct ifreq ifr;

    // get IP address
    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, interface.getLogicalName().c_str());
    ifr.ifr_addr.sa_family = AF_INET;
    if (ioctl(fd, SIOCGIFADDR, &ifr) == 0)
    {
      // IP address is in ifr.ifr_addr
      interface.setConfig("ip", print_ip((sockaddr_in *) (&ifr.ifr_addr)));
      strcpy(ifr.ifr_name, interface.getLogicalName().c_str());
      if ((interface.getConfig("point-to-point") == "yes")
	  && (ioctl(fd, SIOCGIFDSTADDR, &ifr) == 0))
      {
	// remote PPP address is in ifr.ifr_dstaddr
	interface.setConfig("remoteip",
			    print_ip((sockaddr_in *) & ifr.ifr_dstaddr));
      }
    }

    close(fd);
  }
}

static int mii_new_ioctl = -1;

static long mii_get_phy_id(int skfd,
			   hwNode & interface)
{
  struct ifreq ifr;

  memset(&ifr, 0, sizeof(ifr));
  strcpy(ifr.ifr_name, interface.getLogicalName().c_str());
  u16 *data = (u16 *) (&ifr.ifr_data);

  mii_new_ioctl = -1;

  if (ioctl(skfd, 0x8947, &ifr) >= 0)
  {
    mii_new_ioctl = 1;		// new ioctls
  }
  else if (ioctl(skfd, SIOCDEVPRIVATE, &ifr) >= 0)
  {
    mii_new_ioctl = 0;		// old ioctls
  }
  else
  {
    return -1;			// this interface doesn't support ioctls at all
  }

  interface.addCapability("mii");
  return data[0];
}

static int mdio_read(int skfd,
		     int phy_id,
		     int location,
		     hwNode & interface)
{
  struct ifreq ifr;

  memset(&ifr, 0, sizeof(ifr));
  strcpy(ifr.ifr_name, interface.getLogicalName().c_str());
  u16 *data = (u16 *) (&ifr.ifr_data);

  data[0] = phy_id;
  data[1] = location;

  if (ioctl(skfd, mii_new_ioctl ? 0x8948 : SIOCDEVPRIVATE + 1, &ifr) < 0)
    return -1;

  return data[3];
}

static const char *media_names[] = {
  "10bt", "10bt-fd", "100bt", "100bt-fd", "100bt4",
  "flow-control", 0,
};

static bool scan_mii(int fd,
		     hwNode & interface)
{
  long phy_id = mii_get_phy_id(fd, interface);

  if (phy_id < 0)
    return false;

  int mii_reg, i;
  u16 mii_val[32];
  u16 bmcr, bmsr, nway_advert, lkpar;

  for (mii_reg = 0; mii_reg < 8; mii_reg++)
    mii_val[mii_reg] = mdio_read(fd, phy_id, mii_reg, interface);

  if (mii_val[0] == 0xffff || mii_val[1] == 0x0000)	// no MII transceiver present
    return false;

  /*
   * Descriptive rename. 
   */
  bmcr = mii_val[0];
  bmsr = mii_val[1];
  nway_advert = mii_val[4];
  lkpar = mii_val[5];

  if (lkpar & 0x4000)
  {
    int negotiated = nway_advert & lkpar & 0x3e0;
    int max_capability = 0;
    /*
     * Scan for the highest negotiated capability, highest priority
     * (100baseTx-FDX) to lowest (10baseT-HDX). 
     */
    int media_priority[] = { 8, 9, 7, 6, 5 };	/* media_names[i-5] */
    for (i = 0; media_priority[i]; i++)
      if (negotiated & (1 << media_priority[i]))
      {
	max_capability = media_priority[i];
	break;
      }

    if (max_capability)
    {
      switch (max_capability - 5)
      {
      case 0:
	interface.setConfig("autonegociated", "10bt");
	interface.setConfig("duplex", "half");
	break;
      case 1:
	interface.setConfig("autonegociated", "10bt");
	interface.setConfig("duplex", "full");
	break;
      case 2:
	interface.setConfig("autonegociated", "100bt");
	interface.setConfig("duplex", "half");
	break;
      case 3:
	interface.setConfig("autonegociated", "100bt");
	interface.setConfig("duplex", "full");
	break;
      case 4:
	interface.setConfig("autonegociated", "100bt4");
	break;
      }
    }
    else
      interface.setConfig("autonegociated", "none");
  }

  if (bmcr & 0x1000)
    interface.addCapability("autonegotiation");
  else
  {
    if (bmcr & 0x2000)
      interface.setConfig("speed", "100mbps");
    else
      interface.setConfig("speed", "10mbps");

    if (bmcr & 0x0100)
      interface.setConfig("duplex", "full");
    else
      interface.setConfig("duplex", "half");
  }

  if ((bmsr & 0x0016) == 0x0004)
    interface.setConfig("link", "yes");
  else
    interface.setConfig("link", "no");

  if (bmsr & 0xF800)
  {
    for (i = 15; i >= 11; i--)
      if (bmsr & (1 << i))
	interface.addCapability(media_names[i - 11]);
  }

  return true;
}

bool scan_network(hwNode & n)
{
  vector < string > interfaces;

  if (!load_interfaces(interfaces))
    return false;

  int fd = socket(PF_INET, SOCK_DGRAM, 0);

  if (fd > 0)
  {
    struct ifreq ifr;
    struct ethtool_drvinfo drvinfo;

    for (unsigned int i = 0; i < interfaces.size(); i++)
    {
      hwNode interface("network",
		       hw::network);

      interface.setLogicalName(interfaces[i]);
      interface.claim();

      scan_mii(fd, interface);
      scan_ip(interface);

      memset(&ifr, 0, sizeof(ifr));
      strcpy(ifr.ifr_name, interfaces[i].c_str());
      if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0)
      {
#ifdef IFF_PORTSEL
	if (ifr.ifr_flags & IFF_PORTSEL)
	{
	  if (ifr.ifr_flags & IFF_AUTOMEDIA)
	    interface.setConfig("automedia", "yes");
	}
#endif

	if (ifr.ifr_flags & IFF_UP)
	  interface.enable();
	else
	  interface.disable();
	if (ifr.ifr_flags & IFF_BROADCAST)
	  interface.setConfig("broadcast", "yes");
	if (ifr.ifr_flags & IFF_DEBUG)
	  interface.setConfig("debug", "yes");
	if (ifr.ifr_flags & IFF_LOOPBACK)
	  interface.setConfig("loopback", "yes");
	if (ifr.ifr_flags & IFF_POINTOPOINT)
	  interface.setConfig("point-to-point", "yes");
	if (ifr.ifr_flags & IFF_PROMISC)
	  interface.setConfig("promiscuous", "yes");
	if (ifr.ifr_flags & IFF_SLAVE)
	  interface.setConfig("slave", "yes");
	if (ifr.ifr_flags & IFF_MASTER)
	  interface.setConfig("master", "yes");
	if (ifr.ifr_flags & IFF_MULTICAST)
	  interface.setConfig("multicast", "yes");
      }

      memset(&ifr, 0, sizeof(ifr));
      strcpy(ifr.ifr_name, interfaces[i].c_str());
      // get MAC address
      if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0)
      {
	string hwaddr = getmac((unsigned char *) ifr.ifr_hwaddr.sa_data);
	interface.addCapability(hwname(ifr.ifr_hwaddr.sa_family));
	interface.setDescription(string(hwname(ifr.ifr_hwaddr.sa_family)) +
				 " controller");
	interface.setSerial(hwaddr);
      }

      drvinfo.cmd = ETHTOOL_GDRVINFO;
      memset(&ifr, 0, sizeof(ifr));
      strcpy(ifr.ifr_name, interfaces[i].c_str());
      ifr.ifr_data = (caddr_t) & drvinfo;
      if (ioctl(fd, SIOCETHTOOL, &ifr) == 0)
      {
	interface.setConfig("driver", drvinfo.driver);
	interface.setConfig("driverversion", drvinfo.version);
	interface.setConfig("firmware", drvinfo.fw_version);
	interface.setBusInfo(drvinfo.bus_info);
      }

      if (hwNode * existing = n.findChildByBusInfo(interface.getBusInfo()))
      {
	existing->merge(interface);
      }
      else
      {
	existing = n.findChildByLogicalName(interface.getLogicalName());
	if (existing)
	{
	  existing->merge(interface);
	}
	else
	{
	  // we don't care about loopback interfaces
	  if (!interface.isCapable("loopback"))
	    n.addChild(interface);
	}
      }
    }

    close(fd);
    return true;
  }
  else
    return false;
}

static char *id = "@(#) $Id: network.cc,v 1.9 2003/06/28 13:47:40 ezix Exp $";