// Standard Plasma initial layout; all widgets are provided by Plasma itself.
const panel = new Panel;
panel.location = "bottom";
panel.height = 48; // logical pixels: 72 panel pixels at 1.5x, about 7 mm
panel.floating = false;

const launcher = panel.addWidget("org.kde.plasma.kickoff");
launcher.currentConfigGroup = ["General"];
launcher.writeConfig("favorites", ["kitty.desktop", "firefox.desktop", "org.kde.dolphin.desktop", "systemsettings.desktop", "org.kde.kwrite.desktop"]);
launcher.writeConfig("systemFavorites", ["lock-screen", "logout", "reboot", "shutdown"]);
launcher.writeConfig("primaryActions", 2);
launcher.writeConfig("applicationsDisplay", 0);
launcher.writeConfig("compactMode", false);
launcher.writeConfig("switchCategoryOnHover", false);
// Fill most of the 480x480 logical workspace; Plasma clamps popups to the output.
launcher.currentConfigGroup = [];
launcher.writeConfig("popupWidth", 480);
launcher.writeConfig("popupHeight", 432);

const tasks = panel.addWidget("org.kde.plasma.icontasks");
tasks.currentConfigGroup = ["General"];
tasks.writeConfig("launchers", ["applications:kitty.desktop", "applications:firefox.desktop", "applications:org.kde.dolphin.desktop"]);

panel.addWidget("org.kde.plasma.systemtray");
const clock = panel.addWidget("org.kde.plasma.digitalclock");
clock.currentConfigGroup = ["Appearance"];
clock.writeConfig("showDate", false);
clock.writeConfig("showSeconds", 0); // never

for (const desktop of desktopsForActivity(currentActivity())) {
    desktop.wallpaperPlugin = "org.kde.image";
}
