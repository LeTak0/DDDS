# **DDDS – DynDNS DS** 



_A tiny DynDNS updater for Nintendo 3DS/2DS homebrew_

  

DDDS keeps your dynamic-DNS hostname in sync even while the lid is closed.  It wakes every few minutes (you choose how often) and speaks to the most common DynDNS services. 

## WHY? 
I usually host services on my Proxmox/Raspberry Pi cluster, but the cluster was offline during my move—faulty wiring in the new house had to be fixed first. That downtime inspired me to try something different and build this home-brew DynDNS updater for the 3DS. It turned out to be a fun little project.

I wouldn’t necessarily recommend it for mission-critical use just yet, but I’ll keep improving it when I have spare time. Since this is my first real homebrew beyond “hello world,” feel free to leave feedback or open issues—I’m just happy it works for my setup.

---

## **Features**

| **✔︎**                 | **What you get**                                                                                                               |
| ---------------------- | ------------------------------------------------------------------------------------------------------------------------------ |
| **Token & Basic-Auth** | Tested with **DuckDNS** (token). |
| **Always-on Wi-Fi**    | 3DS screen lid can be closed.                                                  |
| **Simple config**      | One file – config.txt – sits next to _DDDS.3dsx_.                                                                              |
| **Tidy logs**          | Successful updates show on-screen with a timestamp.  Failures append to error.log.                        |
| **Polite exit**        | **START** quits.                                                                                                               |

---

## **Quick set-up**

1. **Copy the app**
    
    Place DDDS.3dsx inside sdmc:/3ds/ (any folder inside is fine).
    
2. **Create a folder**
    
    Same place as the app: sdmc:/3ds/ddds/
    
3. **Add config.txt**
    
    Put the file shown below inside that folder.
    
4. **Run!**
    
    Launch DDDS from Homebrew Launcher – you’ll see your entries update.
    

  

### **Example** 

### **config.txt**

```
# Change the interval (in seconds)
interval,600          # update every 10 min

# provider , domain , secret (token **or** Base64 user:pass) , user , pass
# ────────   ────────────────   ───────────────────────────────────────
# DuckDNS (token only)
duckdns , myhome.dns , abcd1234token , ,

# No-IP – put Base64("user:pass") in the secret column
noip , example.ddns.net  , bXl1c2VyOm15cGFzcw== , ,
```

_You can list up to_ **_10_** _hosts.  Blank lines and_ _#_ _comments are ignored._

---

## **Building it yourself**

```
# needs devkitARM + libctru via dkp-pacman
make clean && make          # outputs build/DDDS.3dsx
```

The Makefile only builds a .3dsx and .smdh

---
---

## **License**

Icon : https://www.smashbros.com/wiiu-3ds/images/character/dedede/screen-4.jpg
  

MIT – use it, fork it, ship it.  Credits to the devkitPro & libctru teams for the 3DS homebrew toolchain. 
Thanks to HB Discord kynex7510 and timmskiller
