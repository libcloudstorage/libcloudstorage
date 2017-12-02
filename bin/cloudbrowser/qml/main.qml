import QtQuick 2.7
import QtQuick.Controls 2.0 as Controls
import QtQuick.Layouts 1.2
import org.kde.kirigami 2.0 as Kirigami
import libcloudstorage 1.0

Kirigami.ApplicationWindow {
  property bool visible_player: false
  property bool drawer_state: false
  property bool detailed_options: !android || root.height > root.width

  id: root

  onVisible_playerChanged: {
    if (visible_player) {
      drawer_state = globalDrawer.drawerOpen;
      globalDrawer.drawerOpen = false;
      if (android) android.landscapeOrientation();
      root.showFullScreen();
    } else {
      globalDrawer.drawerOpen = drawer_state;
      if (android) android.defaultOrientation();
      root.showNormal();
    }
  }

  header: visible_player ? null : header

  Kirigami.ApplicationHeader {
    id: header
    width: root.width
  }

  CloudContext {
    property var list_request
    property var currently_moved
    property variant request: []

    id: cloud
    onUserProvidersChanged: {
      root.globalDrawer.actions = root.actions();
    }
    onErrorOccurred: {
      if (operation !== "GetThumbnail")
        root.showPassiveNotification("Error " + code + ": " +
                                     operation + (description ? " " + description : ""));
    }

    function list(title, item) {
      pageStack.push(listDirectoryPage, {title: title, item: item});
    }
  }

  function actions() {
    var ret = [settings.createObject(root.globalDrawer)], i;
    for (i = 0; i < cloud.userProviders.length; i++) {
      var props = {
        provider: cloud.userProviders[i],
        iconName: "qrc:/resources/providers/" + cloud.userProviders[i].type + ".png"
      };
      ret.push(providerAction.createObject(root.globalDrawer, props));
    }
    return ret;
  }

  globalDrawer: Kirigami.GlobalDrawer {
    Component {
      id: settings
      Kirigami.Action {
        text: "Settings"
        Kirigami.Action {
          text: "Add Cloud Provider"
          iconName: "edit-add"
          onTriggered: {
            pageStack.clear();
            pageStack.push(addProviderPage)
          }
        }
        Kirigami.Action {
          text: "Remove Cloud Provider"
          iconName: "edit-delete"
          onTriggered: {
            pageStack.clear();
            pageStack.push(removeProviderPage);
          }
        }
        Kirigami.Action {
          text: "About"
          iconName: "help-about"
          onTriggered: {
            pageStack.clear();
            pageStack.push(aboutView);
          }
        }
      }
    }
    Component {
      id: providerAction
      Kirigami.Action {
        property variant provider
        text: provider.label
        onTriggered: {
          pageStack.clear();
          cloud.currently_moved = null;
          cloud.list(provider.label, cloud.root(provider));
        }
      }
    }

    title: "Cloud Browser"
    titleIcon: "qrc:/resources/cloud.png"
    drawerOpen: true
    actions: root.actions()
  }
  contextDrawer: Kirigami.ContextDrawer {
    id: contextDrawer
  }
  pageStack.initialPage: mainPageComponent
  pageStack.interactive: !visible_player
  pageStack.defaultColumnWidth: 10000

  Component {
    id: addProviderPage
    RegisterPage {
    }
  }

  Component {
    id: removeProviderPage
    RemoveProvider {
    }
  }

  Component {
    id: listDirectoryPage
    ItemPage {
    }
  }

  Component {
    id: aboutView
    AboutView {
    }
  }

  Component {
    id: mainPageComponent
    Kirigami.ScrollablePage {
      anchors.fill: parent
      title: "Select Cloud Provider"
      Item {}
    }
  }

  Component {
    id: upload_component
    UploadItemRequest {
      property string filename
      property bool upload: true
      property var list

      id: upload_request
      onUploadComplete: {
        var lst = [], i;
        for (i = 0; i < cloud.request.length; i++)
          if (cloud.request[i] !== upload_request)
            lst.push(cloud.request[i]);
          else
            upload_request.destroy();
        cloud.request = lst;
        if (list) list.update(cloud, list.item);
      }
    }
  }

  Component {
    id: download_component
    DownloadItemRequest {
      property string filename
      property bool upload: false

      id: download_request
      onDownloadComplete: {
        var lst = [], i;
        for (i = 0; i < cloud.request.length; i++)
          if (cloud.request[i] !== download_request)
            lst.push(cloud.request[i]);
          else
            download_request.destroy();
        cloud.request = lst;
      }
    }
  }

  Component.onCompleted: pageStack.separatorVisible = false
}
