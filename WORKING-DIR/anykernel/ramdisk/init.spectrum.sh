#!/system/bin/sh
# SPECTRUM KERNEL MANAGER
# Profile initialization script by nathanchance

# If there is not a persist value, we need to set one
if [ ! -f /data/property/persist.spectrum.profile ]; then
    setprop persist.spectrum.profile 0
fi

# Permissions,etc
BB=/gabriel/busybox

# protect init from oom
if [ -f /system/xbin/su ]; then
	su -c echo "-1000" > /proc/1/oom_score_adj;
fi;

OPEN_RW()
{
	if [ "$($BB mount | grep rootfs | cut -c 26-27 | grep -c ro)" -ge "0" ]; then
		$BB mount -o remount,rw /;
	fi;
	$BB mount -o remount,rw /system;
}
OPEN_RW;

if [ ! -e /sbin/busybox ];then
	$BB cp /gabriel/busybox /sbin/busybox;
	$BB chmod 06755 /sbin/busybox;
fi;

# some nice thing for dev
if [ ! -e /cpufreq ]; then
	$BB ln -s /sys/devices/system/cpu/cpu0/cpufreq/ /cpufreq_b;
	$BB ln -s /sys/devices/system/cpu/cpu4/cpufreq/ /cpufreq_l;
	$BB ln -s /sys/module/msm_thermal/parameters/ /cputemp;
fi;

# create init.d folder if missing
if [ ! -d /system/etc/init.d ]; then
	mkdir -p /system/etc/init.d/
	$BB chmod 755 /system/etc/init.d/;
fi;

OPEN_RW;

CRITICAL_PERM_FIX()
{
	# critical Permissions fix
	$BB chown -R root:root /sbin;
	$BB chown -R root:root /gabriel;
	$BB chmod 06755 /gabriel/busybox;
	$BB chmod 06755 /system/xbin/busybox;
}
CRITICAL_PERM_FIX;

$BB chmod 666 /sys/module/lowmemorykiller/parameters/cost;
$BB chmod 666 /sys/module/lowmemorykiller/parameters/adj;
$BB chmod 666 /sys/module/lowmemorykiller/parameters/minfree;

$BB chmod 666 /sys/module/msm_thermal/parameters/*

for i in kcal kcal_cont kcal_hue kcal_invert kcal_sat kcal_val; do
	$BB chown system system /sys/devices/platform/kcal_ctrl.0/$i
	$BB chmod 0664 /sys/devices/platform/kcal_ctrl.0/$i
done;

for i in governor max_freq min_freq; do
	$BB chown system system /sys/class/devfreq/1c00000.qcom,kgsl-3d0/$i
	$BB chmod 0664 /sys/class/devfreq/1c00000.qcom,kgsl-3d0/$i
done;

for i in cpu1 cpu2 cpu3 cpu4 cpu5 cpu6 cpu7; do
	$BB chown system system /sys/devices/system/cpu/$i/online;
	$BB chmod 0664 /sys/devices/system/cpu/$i/online;
done;

for i in cpu0 cpu4; do
$BB chown system system /sys/devices/system/cpu/$i/cpufreq/*
$BB chown system system /sys/devices/system/cpu/$i/cpufreq/*
$BB chmod 0664 /sys/devices/system/cpu/$i/cpufreq/scaling_governor;
$BB chmod 0664 /sys/devices/system/cpu/$i/cpufreq/scaling_max_freq;
$BB chmod 0664 /sys/devices/system/cpu/$i/cpufreq/scaling_min_freq;
$BB chmod 0444 /sys/devices/system/cpu/$i/cpufreq/cpuinfo_cur_freq;
$BB chmod 0444 /sys/devices/system/cpu/$i/cpufreq/stats/*
done;

# Miscellaneous configs
echo -n disable > /sys/devices/soc/soc:qcom,bcl/mode;

$BB mount -o remount,ro /system;
