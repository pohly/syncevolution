pref("toolkit.defaultChromeURI", "chrome://bdb-xul/content/bdb.xul");
pref("browser.chromeURL", "chrome://bdb-xul/content/bdb.xul");

# Debug options
pref("browser.dom.window.dump.enabled", true);
pref("javascript.options.showInConsole", true);
pref("javascript.options.strict", true);
pref("nglayout.debug.disable_xul_cache", true);
pref("nglayout.debug.disable_xul_fastload", true);
user_pref("browser.shell.checkDefaultBrowser", false);

# Hack to automatically allow local URLs to have priviliged access
user_pref("signed.applets.codebase_principal_support", true);
user_pref("capability.principal.codebase.p0.granted", "UniversalXPConnect");
user_pref("capability.principal.codebase.p0.id", "file://");
user_pref("capability.principal.codebase.p0.subjectName", "");
