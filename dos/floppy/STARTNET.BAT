@echo off
vmnic
vmpkt
vmipx
REM TCP/IP, IPv4 and IPv6 (A:\VMDOS\NET: VMTCP, DHCP, PING, NSLOOKUP).
REM The settings are in VMTCP.CFG, which DHCP fills in.
REM VMIPXBOX (the DOSBox IPX bridge) needs the vmipx line left out.
IF "%MTCPCFG%"=="" PATH %PATH%;A:\VMDOS\NET
IF "%MTCPCFG%"=="" SET MTCPCFG=A:\VMDOS\NET\VMTCP.CFG
IF NOT EXIST %MTCPCFG% VMTCP PACKETINT 0x60 >NUL
DHCP -timeout 5 -retries 2
