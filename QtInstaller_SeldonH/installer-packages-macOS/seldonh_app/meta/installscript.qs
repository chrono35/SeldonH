/**************************************************************************
** SeldonH - installscript.qs (composant seldonh_app)
** Raccourcis : menu Démarrer + Bureau sous Windows, entrée de menu sous Linux.
** Serveur MCP : chemin réel de seldonh-mcp écrit dans LISEZMOI_MCP.txt.
**************************************************************************/

function Component()
{
}

Component.prototype.createOperations = function()
{
    component.createOperations();   // copie des fichiers

    // Serveur MCP (seldonh-mcp) : le LISEZMOI donne le chemin à recopier dans la configuration de l'assistant
    // (Claude Desktop, Jan, Gemini CLI). Barres obliques partout : valables dans un JSON et sous Windows.
    var target = installer.value("TargetDir").replace(/\\/g, "/");
    var mcp = target + (systemInfo.productType === "windows" ? "/seldonh-mcp.exe"
                      : systemInfo.kernelType === "darwin" ? "/SeldonH.app/Contents/MacOS/seldonh-mcp"
                      : "/seldonh-mcp");
    component.addOperation("Replace", target + "/LISEZMOI_MCP.txt", "{{SELDONH_MCP}}", mcp);

    if (systemInfo.productType === "windows") {
        component.addOperation("CreateShortcut",
            "@TargetDir@/SeldonH.exe", "@StartMenuDir@/SeldonH.lnk",
            "workingDirectory=@TargetDir@", "iconPath=@TargetDir@/SeldonH.exe", "iconId=0",
            "description=Assistant de datation pour ChronoModel");
        component.addOperation("CreateShortcut",
            "@TargetDir@/SeldonH.exe", "@DesktopDir@/SeldonH.lnk",
            "workingDirectory=@TargetDir@", "iconPath=@TargetDir@/SeldonH.exe", "iconId=0");
        component.addOperation("CreateShortcut",
            "@TargetDir@/SeldonH_uninstaller.exe", "@StartMenuDir@/Désinstaller SeldonH.lnk",
            "workingDirectory=@TargetDir@");
    }
    else if (systemInfo.kernelType === "linux") {
        component.addOperation("CreateDesktopEntry",
            "@HomeDir@/.local/share/applications/SeldonH.desktop",
            "Type=Application\nName=SeldonH\nComment=Assistant de datation pour ChronoModel\n"
            + "Exec=@TargetDir@/SeldonH.sh %F\nIcon=@TargetDir@/seldonh.png\nTerminal=false\n"
            + "Categories=Science;Education;");
    }
}
