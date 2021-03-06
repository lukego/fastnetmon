### Here we store all code related with Snabb switch intergration :) We like it because it's awesome!

First of all, please compile Snabb Switch:
```bash
cd /usr/src/
git clone https://github.com/SnabbCo/snabbswitch.git
cd snabbswitch
make
```

Then compile our .so library:
```
gcc -shared -o capturecallback.so -fPIC capturecallback.c
```

Unload all ixgbe drivers, pf_ring and netmap explicitly:
```bash
rmmod ixgbe netmap pf_ring
rmmod ixgbe netmap pf_ring
```

Get NIC's PCI address:
```bash
lspci -m|grep 82599
00:05.0 "Ethernet controller" "Intel Corporation" "82599ES 10-Gigabit SFI/SFP+ Network Connection" -r01 "Intel Corporation" "Ethernet Server Adapter X520-2"
00:06.0 "Ethernet controller" "Intel Corporation" "82599ES 10-Gigabit SFI/SFP+ Network Connection" -r01 "Intel Corporation" "Ethernet Server Adapter X520-2"
```

For example, we will use 00:06.0, we need convert this value to Snabb's format with adding 4 leading zeroes: 0000:00:06.0

Run capture:
```bash
/usr/src/snabbswitch/src/snabb snsh capture2c.lua 0000:00:06.0 ./capturecallback.so
```

I have got really amazing results:
```bash
Loading shared object: ./capturecallback.so
Initializing NIC: 0000:00:06.0
Run speed printer from C code
Processing traffic...

We process: 14566196 pps
We process: 14820487 pps
We process: 14856881 pps
We process: 14863727 pps
```

We achieved this with only single logical core of i7 3820 CPU:
```bash
1  [                                                             0.0%]     
5  [                                                             0.0%]  
2  [                                                             0.0%]     
6  [                                                             0.0%]
3  [                                                             0.0%]     
7  [|||||||||||||||||||||||||||||||||||||||||||||||||||||||||||100.0%]
4  [                                                             0.0%]     
8  [                                                             0.0%]
```

Really awesome! Huge thanks to Luke Gorrie for great help with it!
