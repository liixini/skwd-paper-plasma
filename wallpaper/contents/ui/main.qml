import QtQuick
import QtQuick.Window
import org.kde.plasma.plasmoid
import org.skwd.wallpaper 1.0
import org.kde.taskmanager as TaskManager

WallpaperItem {
    id: root

    function parseAssignments(raw) {
        if (!raw)
            return ({})
        try {
            return JSON.parse(raw)
        } catch (error) {
            return ({})
        }
    }

    property var assignments: parseAssignments(configuration.Assignments || "")
    property var currentAssignment: assignments[Screen.name] || null

    TaskManager.VirtualDesktopInfo { id: desktopInfo }
    TaskManager.ActivityInfo { id: activityInfo }
    TaskManager.TasksModel {
        id: windowTasks
        groupMode: TaskManager.TasksModel.GroupDisabled
        filterByVirtualDesktop: true
        virtualDesktop: desktopInfo.currentDesktop
        filterByActivity: true
        activity: activityInfo.currentActivity
        filterByScreen: true
        screenGeometry: Qt.rect(root.Screen.virtualX, root.Screen.virtualY, root.Screen.width, root.Screen.height)
        filterMinimized: true
        filterHidden: true
    }
    SkwdWindowMonitor {
        id: windowMonitor
        model: windowTasks
        output: root.Screen.name
    }

    SkwdVideoItem {
        anchors.fill: parent
        presentationId: root.currentAssignment ? (root.currentAssignment.presentationId || "") : ""
        assignment: root.currentAssignment
            ? JSON.stringify(root.currentAssignment.assignment)
            : (configuration.Assignment || "")
        paper: root.currentAssignment
            ? root.currentAssignment.paper
            : (configuration.Paper || "skwd-paper-v2")
        streamWidth: root.width > 0
            ? Math.min(Math.round(root.width * Math.max(1, Screen.devicePixelRatio)),
                       root.currentAssignment ? root.currentAssignment.width : Infinity)
            : (root.currentAssignment ? root.currentAssignment.width : (configuration.StreamWidth || 1920))
        streamHeight: root.height > 0
            ? Math.min(Math.round(root.height * Math.max(1, Screen.devicePixelRatio)),
                       root.currentAssignment ? root.currentAssignment.height : Infinity)
            : (root.currentAssignment ? root.currentAssignment.height : (configuration.StreamHeight || 1080))
        streamFps: Screen.refreshRate > 0
            ? Math.min(root.currentAssignment ? root.currentAssignment.fps : (configuration.StreamFps || 30), Math.round(Screen.refreshRate))
            : (root.currentAssignment ? root.currentAssignment.fps : (configuration.StreamFps || 30))
        paused: windowMonitor.hasPolicy ? windowMonitor.paused
            : (root.currentAssignment
                ? (typeof root.currentAssignment.manualPaused === "boolean"
                    ? root.currentAssignment.manualPaused : root.currentAssignment.paused)
                : (configuration.Paused || false))
    }

    Component.onCompleted: root.loading = false
}
