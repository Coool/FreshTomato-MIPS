<!DOCTYPE HTML PUBLIC '-//W3C//DTD HTML 4.0//EN'>
<!--
	Tomato GUI
	Copyright (C) 2006-2010 Jonathan Zarate
	http://www.polarcloud.com/tomato/

	For use with Tomato Firmware only.
	No part of this file may be used without permission.
-->
<html>
<head>
<meta http-equiv='content-type' content='text/html;charset=utf-8'>
<meta name='robots' content='noindex,nofollow'>
<title>[<% ident(); %>] About</title>
<link rel='stylesheet' type='text/css' href='tomato.css'>
<% css(); %>
<script type='text/javascript' src='tomato.js'></script>
<script type='text/javascript'>
//	<% nvram(''); %>	// http_id

var clicks = 0;
var tux = null;
var t = 0;
var r = 3;
var rd = 1;

function moo()
{
	if ((r <= 2) || (r >= 25)) rd = -rd;
	r += rd;
	t += (Math.PI / 10);
	if (t > (2 * Math.PI)) t = 0;

	var x = tux.origX + (r * Math.sin(t));
	var y = tux.origY + (r * Math.cos(t));

	tux.style.left = x + 'px';
	tux.style.top = y + 'px';

	if (clicks > 0) setTimeout(moo, 33);
}

function onClick()
{
	try {
		++clicks;
		if (clicks < 10) moo();
			else clicks = 0;
	}
	catch (ex) {
	}
}

function init()
{
	try {
		tux = E('tux');

		var o = elem.getOffset(tux);
		tux.origX = o.x;
		tux.origY = o.y;

		tux.style.position = 'absolute';
		tux.style.left = o.x + 'px';
		tux.style.top = o.y + 'px';

		tux.addEventListener('click', onClick, false);
	}
	catch (ex) {
	}
}
</script>

<!-- / / / -->

</head>
<body onload='init()'>
<table id='container' cellspacing=0>
<tr><td colspan=2 id='header'>
	<div class='title'>Tomato</div>
	<div class='version'>Version <% version(); %></div>
</td></tr>
<tr id='body'><td id='navi'><script type='text/javascript'>navi()</script></td>
<td id='content'>
<div id='ident'><% ident(); %></div>

<!-- / / / -->

<div style='float:right;margin:140px 30px;text-align:center'>
<img src='kinaree.png' alt='Linux &amp; Tomato' id='tux'>
</div>
<div style='margin:30px 30px;font-size:14px;color:#555;'>
<b>Tomato Firmware v<% version(1); %></b><br>
<br>
Based on TomatoUSB by Fedor Kozhevnikov<br>
- Linux kernel <% version(2); %> and Broadcom Wireless Driver <% version(3); %> updates<br>
<i>Copyright (C) 2013-2015 Tomato-ARM Team</i><br>
<br>
<b>Tomato-ARM Team:</b><br>
- Michał Rupental (Shibby)<br>
- Ofer Chen (roadkill)<br>
- Vicente Soriano (Victek)<br>
<br>
<hr>
<!-- / / / -->

<br>
<b>TomatoUSB Team features:</b><br>
<!-- USB-BEGIN -->
- USB support integration and GUI<br>
<!-- USB-END -->
<!-- IPV6-BEGIN -->
- IPv6 support<br>
<!-- IPV6-END -->
- Dual-band and Wireless-N mode<br>
<i>Copyright (C) 2008-2011 Fedor Kozhevnikov, Ray Van Tassle, Wes Campaigne</i><br>
<a href='http://www.tomatousb.org/' target='_new'>http://www.tomatousb.org</a><br>
<br>
<div style='border-top:2px solid #e7e7e7;margin:1em 0;padding:1em 0;font-size:12px'>
</div>
<b>This compilation by Toastman may include:</b><br>
<br>
<br>
<b>"Shibby" features:</b><br>
<!-- BBT-BEGIN -->
- Transmission 2.84 integration<br>
<!-- BBT-END -->
<!-- BT-BEGIN -->
- GUI for Transmission<br>
<!-- BT-END -->
<!-- NFS-BEGIN -->
- NFS utils integration and GUI<br>
<!-- NFS-END -->
- Custom log file path<br>
<!-- LINUX26-BEGIN -->
- SD-idle tool integration for kernel 2.6<br>
<!-- USB-BEGIN -->
- 3G Modem support (big thanks for @LDevil)<br>
- 4G/LTE Modem support<br>
<!-- USB-END -->
<!-- LINUX26-END -->
<!-- SNMP-BEGIN -->
- SNMP integration and GUI<br>
<!-- SNMP-END -->
<!-- UPS-BEGIN -->
- APCUPSD integration and GUI (implemented by @arrmo)<br>
<!-- UPS-END -->
<!-- DNSCRYPT-BEGIN -->
- DNScrypt-proxy 1.4.0 integration and GUI<br>
<!-- DNSCRYPT-END -->
<!-- TOR-BEGIN -->
- TOR Project integration and GUI<br>
<!-- TOR-END -->
<!-- OPENVPN-BEGIN -->
- OpenVPN: Routing Policy
<!-- OPENVPN-END -->
- TomatoThemeBase project integration and GUI<br>
- Ethernet Ports State<br>
- Extended MOTD (written by @Monter, modified by @Shibby)<br>
- Webmon Backup Script<br>
<i>Copyright (C) 2011-2014 Michał Rupental</i><br>
<a href='http://openlinksys.info' target='_new'>http://openlinksys.info</a><br>
<br>

<b>Tomato-RAF features:</b><br>
- Extended Sysinfo<br>
<!-- NOCAT-BEGIN -->
- Captive Portal. (Based in NocatSplash)<br>
<!-- NOCAT-END -->
<!-- NGINX-BEGIN -->
- Web Server. (NGinX)<br>
<!-- NGINX-END -->
<!-- HFS-BEGIN -->
- HFS / HFS+ filesystem integration<br>
<!-- HFS-END -->
<i>Copyright (C) 2007-2014 Ofer Chen & Vicente Soriano</i><br>
<a href='http://victek.is-a-geek.com' target='_new'>http://victek.is-a-geek.com</a><br>
<br>

<b>"Teaman" features:</b><br>
- QOS-detailed & ctrate filters<br>
- Realtime bandwidth monitoring of LAN clients<br>
- Static ARP binding<br>
- VLAN administration GUI<br>
- Multiple LAN support integration and GUI<br>
- Multiple/virtual SSID support (experimental)<br>
- UDPxy integration and GUI<br>
<!-- PPTPD-BEGIN -->
- PPTP Server integration and GUI<br>
<!-- PPTPD-END -->
<i>Copyright (C) 2011 Augusto Bott</i><br>
<a href='http://code.google.com/p/tomato-sdhc-vlan/' target='_new'>Tomato-sdhc-vlan Homepage</a><br>
<br>

<b>"Lancethepants" features:</b><br>
<!-- DNSSEC-BEGIN -->
- DNSSEC integration and GUI<br>
<!-- DNSSEC-END -->
<!-- DNSCRYPT-BEGIN -->
- DNSCrypt-Proxy selectable/manual resolver<br>
<!-- DNSCRYPT-END -->
<!-- TINC-BEGIN -->
- Tinc Daemon integration and GUI<br>
<!-- TINC-END -->
- Comcast DSCP Fix GUI<br>
<i>Copyright (C) 2014 Lance Fredrickson</i><br>
<a href='mailto:lancethepants@gmail.com'>lancethepants@gmail.com</a><br>
<br>

<!-- VPN-BEGIN -->
<b>"JYAvenard" features:</b><br>
<!-- OPENVPN-BEGIN -->
- OpenVPN enhancements &amp; username/password only authentication<br>
<!-- OPENVPN-END -->
<!-- PPTPD-BEGIN -->
- PPTP VPN Client integration and GUI<br>
<!-- PPTPD-END -->
<i>Copyright (C) 2010-2012 Jean-Yves Avenard</i><br>
<a href='mailto:jean-yves@avenard.org'>jean-yves@avenard.org</a><br>
<br>

<!-- OPENVPN-BEGIN -->
<b>TomatoVPN feature:</b><br>
- OpenVPN integration and GUI<br>
<i>Copyright (C) 2010 Keith Moyer</i><br>
<a href='mailto:tomatovpn@keithmoyer.com'>tomatovpn@keithmoyer.com</a><br>
<br>

<b>"TomatoEgg" feature:</b><br>
- Openvpn username/password verify feature and configure GUI.<br>
<br>
<!-- OPENVPN-END -->
<!-- VPN-END -->

<!-- NGINX-BEGIN -->
<b>Tomato-hyzoom feature:</b><br>
- MySQL Server integration and GUI<br>
<i>Copyright (C) 2014 Bao Weiquan, Hyzoom</i><br>
<a href='mailto:bwq518@gmail.com'>bwq518@gmail.com</a><br>
<br>
<!-- NGINX-END -->

<b>"Toastman" Features:</b><br>
- 250 entry limit in Static DHCP  & Wireless Filter<br>
- 500 entry limit in Access Restriction rules*<br>
- Up to 128 Port Forward rules*<br>
- Up to 128 QOS rules* (*all dependent on nvram space)<br>
- Configurable QOS class names<br>
- Comprehensive QOS rule examples set by default<br>
Copyright (C) 2010-2015 Toastman<br>
<a href='http://www.toastmanfirmware.yolasite.com'>http://www.toastmanfirmware.yolasite.com</a><br>
<br>
Built on <% build_time(); %><br>
<br>
<b>If you enjoy this firmware, and would like to thank me for all the time I spent<br>
working on it, you can make a PayPal donation:</b>
<br>
<br>
<form action="http://toastmanfirmware.yolasite.com/donations.php" method="post">
<input type="image" src="https://www.paypalobjects.com/en_US/i/btn/btn_donateCC_LG.gif" border="0" name="submit">
</form>
<br>
<br>
<div style='border-top:2px solid #e7e7e7;margin:1em 0;padding:1em 0;font-size:12px'>
</div>
<b>Based on Tomato Firmware v<% version(); %></b><br>
Copyright (C) 2006-2010 Jonathan Zarate<br>
<a href='http://www.polarcloud.com/tomato/' target='_new'>http://www.polarcloud.com/tomato/</a><br>
<br>
<!--

	Please do not remove or change the homepage link or donate button.

	Thanks.
	- Jon

-->

<form action="https://www.paypal.com/cgi-bin/webscr" method="post">
<input type="hidden" name="cmd" value="_s-xclick">
<input type="image" src="https://www.paypalobjects.com/en_US/i/btn/btn_donateCC_LG.gif" border="0" name="submit" alt="Donate">
<input type="hidden" name="encrypted" value="-----BEGIN PKCS7-----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-----END PKCS7-----">
</form>

<div style='border-top:2px solid #e7e7e7;margin:2em 0;padding:2em 0;font-size:12px'>
<b>Thanks to everyone who risked their routers, tested, reported bugs, made
suggestions and contributed to this project. ^ _ ^</b><br>
</div>

</div>


</td></tr>
	<tr><td id='footer' colspan=2>&nbsp;</td></tr>
</table>
</body>
</html>