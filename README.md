# obs-indicator
obs recording status indicator that doesn't make your games stutter

Once indicator is shown, it has 0% effect on frame times in my test (approximating real on-screen frame times as much as possible via msBetweenDisplayChange in RTSS Overlay Editor):

<img width="198" height="124" alt="nostutter" src="https://github.com/user-attachments/assets/ff60af9a-dc45-4533-a771-114ad5fb076d" />

---

<img width="482" height="324" alt="obsindicatorsettings" src="https://github.com/user-attachments/assets/3427fffa-64d6-4875-af44-883fc21dac8a" />


Program draws top-most window as efficiently as possible (presumably). Minimizes to tray, auto-starts to tray as well.

You need to enable WebSocket server in obs (Tools -> WebSocket Server Settings). Supports password authentication, saves password encrypted in config and should not unnecessarily keep unencrypted password in RAM.
