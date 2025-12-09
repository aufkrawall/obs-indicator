# obs-indicator
obs recording status indicator that doesn't make your games stutter

Once steady indicator is shown, it has 0% effect on frame times in my test (approximating real on-screen frame times as much as possible via msBetweenDisplayChange in RTSS Overlay Editor):

<img width="198" height="124" alt="nostutter" src="https://github.com/user-attachments/assets/ff60af9a-dc45-4533-a771-114ad5fb076d" />

---

<img width="482" height="395" alt="obsindicator" src="https://github.com/user-attachments/assets/ad6fbbcf-230a-4d98-8542-d2d503727c2d" />

Program draws top-most window as efficiently as possible (presumably). Minimizes to tray, auto-starts to tray as well.

Now also has warning detection when obs recording/streaming IS NOT running in your game, e.g. as a reminder between different matches. You must add your game to process list for this, e.g. FortniteClient-Win64-Shipping.exe (is just running process detection and still 100% safe with anti-cheat, no inject of any kind).

You need to enable WebSocket server in obs (Tools -> WebSocket Server Settings), the tool is retrieving obs status via that function. Supports password authentication, saves password encrypted in config and should not unnecessarily keep unencrypted password in RAM. Might be AI generated.
