import QtQuick
import Components

FocusScope {
    id: signInRoot

    signal goBack()

    focus: true

    property var navParams: ({})

    // Issue #28 plan D (relocated per Ian): pairing lives ONLY here — the
    // module Settings "Sign In on YouTube" row opens this subview, and
    // opening it IS the entry point: the device flow starts on open, the
    // code stays on screen until the user approves it on their own device
    // or presses BACK (which cancels the poll). Once signed in, this same
    // subview is the exit door: it shows the state and offers SELECT for
    // sign-out. Root.qml no longer carries any pairing surface.
    //
    // No timed auto-clear anywhere (an earlier draft used Qt.setTimeout —
    // not a QML global, TypeError on the Pi): every state here lives until
    // the user leaves on their own input.
    property string pairCode: ""
    property string pairUrl: ""
    property bool pairPending: false
    property string notice: ""
    property bool signedInNow: false
    property bool signOutRequested: false

    Component.onCompleted: {
        var status = youtubeBackend.check_signin()
        if (status.signedIn === true) {
            signedInNow = true
        } else if (status.pairingActive === true) {
            // A flow still polling from an earlier open: re-read its pair
            // state so the screen shows the LIVE code, not a fresh one.
            pairPending = true
            pairCode = youtubeBackend.pairUserCode()
            pairUrl = youtubeBackend.pairVerificationUrl()
        } else {
            youtubeBackend.startDeviceSignIn()
            pairPending = true
        }
    }

    Connections {
        target: youtubeBackend

        function onDevicePairRequested(userCode, url) {
            signInRoot.pairCode = userCode
            signInRoot.pairUrl = url
        }
        function onDeviceSignInFinished(ok, reason) {
            if (ok) {
                signInRoot.pairPending = false
                signInRoot.signedInNow = true
                signInRoot.notice = ""
                return
            }
            if (signInRoot.signOutRequested) {
                // Our own sign-out request round-trips through this signal.
                signInRoot.signOutRequested = false
                signInRoot.signedInNow = false
                signInRoot.notice = ""
                return
            }
            if (signInRoot.pairPending) {
                // Declined or code expired — surface it; user backs out when ready.
                signInRoot.pairPending = false
                signInRoot.notice = reason.toUpperCase().replace(" ", "-")
                return
            }
            signInRoot.notice = reason.toUpperCase().replace(" ", "-")
        }
        function onErrorOccurred(msg) {
            signInRoot.notice = msg
        }
    }

    // ---
    // Header
    // ---
    AppBar {
        iconSource: "../assets/images/logo.svg"
        title: "YOUTUBE"
        subtitle: "SIGN IN ON YOUTUBE"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.125
        anchors.leftMargin: root.sw * 0.125
    }

    // ---
    // Status line: exactly one headline at a time.
    // ---
    Text {
        id: statusLine
        text: signInRoot.signedInNow ? "SIGNED IN"
              : signInRoot.notice !== "" ? signInRoot.notice
              : signInRoot.pairPending ? "PAIRING..."
              : "NOT SIGNED IN"
        color: signInRoot.notice !== "" ? root.secondaryColor : root.accentColor
        font.family: root.globalFont
        font.pixelSize: root.sh * 0.05
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: root.sh * 0.28
        width: root.sw * 0.62
        wrapMode: Text.WordWrap
        horizontalAlignment: Text.AlignHCenter
    }

    // Pairing detail: URL + code + wait guidance, present while polling.
    Column {
        id: pairingPanel
        visible: signInRoot.pairPending
        anchors.top: statusLine.bottom
        anchors.topMargin: root.sh * 0.06
        anchors.horizontalCenter: parent.horizontalCenter
        width: root.sw * 0.62
        spacing: root.sh * 0.03

        Text {
            text: "OPEN " + (signInRoot.pairUrl !== ""
                              ? signInRoot.pairUrl : "HTTPS://WWW.GOOGLE.COM/DEVICE")
            color: root.secondaryColor
            font.family: root.globalFont
            font.pixelSize: root.sh * 0.03
            width: pairingPanel.width
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }
        Text {
            text: signInRoot.pairCode !== "" ? "TYPE THIS CODE: " + signInRoot.pairCode
                                             : "WAITING FOR A CODE..."
            color: root.primaryColor
            font.family: root.globalFont
            font.pixelSize: root.sh * 0.055
            horizontalAlignment: Text.AlignHCenter
        }
        Text {
            text: "TYPE IT ON ANY DEVICE — THIS SCREEN WAITS UNTIL YOU APPROVE IT"
            color: root.tertiaryColor
            font.family: root.globalFont
            font.pixelSize: root.sh * 0.028
            width: pairingPanel.width
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // Signed-in exit door: SELECT on SIGN OUT performs sign-out (stays on
    // this screen showing the result); STAY SIGNED IN leaves without changes.
    RowTrackingListView {
        id: signOutList
        visible: signInRoot.signedInNow
        model: signInRoot.signedInNow
               ? [{ label: "SIGN OUT" }, { label: "STAY SIGNED IN" }]
               : []
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: root.sh * 0.42
        anchors.leftMargin: root.sw * 0.28
        width: root.sw * 0.44
        height: root.sh * 0.14
        clip: true
        focus: true

        Keys.onUpPressed: {
            if (count === 0) return
            if (currentIndex > 0) currentIndex--
            else currentIndex = count - 1
            signOutList.positionViewAtIndex(signOutList.currentIndex, ListView.Contain)
        }
        Keys.onDownPressed: {
            if (count === 0) return
            if (currentIndex < count - 1) currentIndex++
            else currentIndex = 0
            signOutList.positionViewAtIndex(signOutList.currentIndex, ListView.Contain)
        }

        Keys.onReturnPressed: {
            var choice = signOutList.currentIndex === 0 ? "sign-out" : "stay"
            if (choice === "sign-out") {
                signInRoot.signOutRequested = true
                youtubeBackend.deviceSignOut()
            } else {
                goBack()
            }
        }

        delegate: Item {
            width: signOutList.width
            height: root.sh * 0.0583333

            Rectangle {
                anchors.fill: parent
                color: signOutList.currentIndex === index ? root.accentColor : "transparent"

                Text {
                    text: modelData.label || ""
                    color: signOutList.currentIndex === index ? root.surfaceColor : root.primaryColor
                    font.family: root.globalFont
                    font.capitalization: Font.AllUppercase
                    anchors.verticalCenter: parent.verticalCenter
                    x: 0
                    topPadding: root.sh * 0.0041667
                    leftPadding: root.sw * 0.009375
                    rightPadding: root.sw * 0.009375
                    bottomPadding: root.sh * 0.00625
                    font.pixelSize: root.sh * 0.05
                }
            }
        }
    }

    // ---
    // Footer
    // ---
    Text {
        id: footer
        text: root.hints.back + ":BACK " + (signInRoot.signedInNow ? root.hints.select + ":SELECT" : "")
        color: root.tertiaryColor
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.sh * 0.1041667
        anchors.leftMargin: root.sw * 0.125
        anchors.left: parent.left
        font.family: root.globalFont
        font.pixelSize: root.sh * 0.0333333
    }

    // BACK leaves the subview; a poll still in flight is cancelled with it,
    // so the account never ends up half-paired behind the user's back.
    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Backspace || event.key === Qt.Key_Back) {
            if (signInRoot.pairPending) {
                signInRoot.pairPending = false
                youtubeBackend.cancelDeviceSignIn()
            }
            goBack()
            event.accepted = true
        }
    }
}
