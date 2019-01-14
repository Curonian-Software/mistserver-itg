mistplayers.webrtc={name:"WebRTC player",mimes:["webrtc"],priority:MistUtil.object.keys(mistplayers).length+1,isMimeSupported:function(e){return this.mimes.indexOf(e)==-1?false:true},isBrowserSupported:function(e,t,n){if(!("WebSocket"in window)||!("RTCPeerConnection"in window)){return false}if(location.protocol.replace(/^http/,"ws")!=MistUtil.http.url.split(t.url.replace(/^http/,"ws")).protocol){n.log("HTTP/HTTPS mismatch for this source");return false}return true},player:function(){}};var p=mistplayers.webrtc.player;p.prototype=new MistPlayer;p.prototype.build=function(e,t){var n=this;if(typeof WebRTCBrowserEqualizerLoaded=="undefined"||!WebRTCBrowserEqualizerLoaded){var i=document.createElement("script");i.src=e.urlappend(e.options.host+"/webrtc.js");e.log("Retrieving webRTC browser equalizer code from "+i.src);document.head.appendChild(i);i.onerror=function(){e.showError("Failed to load webrtc browser equalizer",{nextCombo:5})};i.onload=function(){n.build(e,t)};return}var r=document.createElement("video");var o=["autoplay","loop","poster"];for(var s in o){var c=o[s];if(e.options[c]){r.setAttribute(c,e.options[c]===true?"":e.options[c])}}if(e.options.muted){r.muted=true}if(e.info.type=="live"){r.loop=false}if(e.options.controls=="stock"){r.setAttribute("controls","")}r.setAttribute("crossorigin","anonymous");this.setSize=function(e){r.style.width=e.width+"px";r.style.height=e.height+"px"};MistUtil.event.addListener(r,"loadeddata",v);MistUtil.event.addListener(r,"seeked",v);var a=0;var u=false;this.listeners={on_connected:function(){a=0;u=false;this.webrtc.play()},on_disconnected:function(){e.log("Websocket sent on_disconnect");if(u){e.showError("Connection to media server ended unexpectedly.")}r.pause()},on_answer_sdp:function(t){if(!t.result){e.showError("Failed to open stream.");this.on_disconnected();return}e.log("SDP answer received")},on_time:function(e){var t=a;a=e.current*.001-r.currentTime;if(Math.abs(t-a)>1){v()}var n=e.end==0?Infinity:e.end*.001;if(n!=d){d=n;MistUtil.event.send("durationchange",n,r)}},on_seek:function(){MistUtil.event.send("seeked",a,r);r.play()},on_stop:function(){e.log("Websocket sent on_stop");MistUtil.event.send("ended",null,r);u=true}};function f(){this.peerConn=null;this.localOffer=null;this.isConnected=false;var t=this;this.on_event=function(i){switch(i.type){case"on_connected":{t.isConnected=true;break}case"on_answer_sdp":{t.peerConn.setRemoteDescription({type:"answer",sdp:i.answer_sdp}).then(function(){},function(e){console.error(e)});break}case"on_disconnected":{t.isConnected=false;break}}if(i.type in n.listeners){return n.listeners[i.type].call(n,i)}e.log("Unhandled WebRTC event "+i.type+": "+JSON.stringify(i));return false};this.connect=function(e){t.signaling=new p(t.on_event);t.peerConn=new RTCPeerConnection;t.peerConn.ontrack=function(t){r.srcObject=t.streams[0];if(e){e()}}};this.play=function(){if(!this.isConnected){throw"Not connected, cannot play"}this.peerConn.createOffer({offerToReceiveAudio:true,offerToReceiveVideo:true}).then(function(e){t.localOffer=e;t.peerConn.setLocalDescription(e).then(function(){t.signaling.sendOfferSDP(t.localOffer.sdp)},function(e){console.error(e)})},function(e){throw e})};this.stop=function(){if(!this.isConnected){throw"Not connected, cannot stop."}this.signaling.send({type:"stop"})};this.seek=function(e){if(!this.isConnected){return}this.signaling.send({type:"seek",seek_time:e*1e3})};this.pause=function(){if(!this.isConnected){throw"Not connected, cannot pause."}this.signaling.send({type:"pause"})};this.setTrack=function(e){if(!this.isConnected){throw"Not connected, cannot set track."}e.type="tracks";this.signaling.send(e)};this.getStats=function(e){this.peerConn.getStats().then(function(t){var n={};var i=Array.from(t.entries());for(var r in i){var o=i[r];if(o[1].type=="inbound-rtp"){n[o[0]]=o[1]}}e(n)})};this.connect()}function p(t){this.ws=null;this.ws=new WebSocket(e.source.url.replace(/^http/,"ws"));this.ws.onopen=function(){t({type:"on_connected"})};this.ws.onmessage=function(e){try{var n=JSON.parse(e.data);t(n)}catch(t){console.error("Failed to parse a response from MistServer",t,e.data)}};this.ws.onclose=function(e){switch(e.code){default:{t({type:"on_disconnected"});break}}};this.sendOfferSDP=function(e){this.send({type:"offer_sdp",offer_sdp:e})};this.send=function(e){if(!this.ws){throw"Not initialized, cannot send "+JSON.stringify(e)}this.ws.send(JSON.stringify(e))}}this.webrtc=new f;this.api={};var d;Object.defineProperty(this.api,"duration",{get:function(){return d}});Object.defineProperty(this.api,"currentTime",{get:function(){return a+r.currentTime},set:function(e){a=e-r.currentTime;r.pause();n.webrtc.seek(e);MistUtil.event.send("seeking",e,r)}});function l(e){Object.defineProperty(n.api,e,{get:function(){return r[e]},set:function(t){return r[e]=t}})}var h=["volume","muted","loop","paused",,"error","textTracks","webkitDroppedFrameCount","webkitDecodedFrameCount"];for(var s in h){l(h[s])}function w(e){if(e in r){n.api[e]=function(){return r[e].call(r,arguments)}}}var h=["load","getVideoPlaybackQuality"];for(var s in h){w(h[s])}n.api.play=function(){if(n.api.currentTime){if(!n.webrtc.isConnected||n.webrtc.peerConn.iceConnectionState!="completed"){n.webrtc.connect(function(){n.webrtc.seek(n.api.currentTime)})}else{n.webrtc.seek(n.api.currentTime)}}else{r.play()}};n.api.pause=function(){r.pause();try{n.webrtc.pause()}catch(e){}MistUtil.event.send("paused",null,r)};n.api.setTracks=function(e){n.webrtc.setTrack(e)};function v(){if(!n.api.textTracks[0]){return}var e=n.api.textTracks[0].currentOffset||0;if(Math.abs(a-e)<1){return}var t=[];for(var i=n.api.textTracks[0].cues.length-1;i>=0;i--){var r=n.api.textTracks[0].cues[i];n.api.textTracks[0].removeCue(r);if(!("orig"in r)){r.orig={start:r.startTime,end:r.endTime}}r.startTime=r.orig.start-a;r.endTime=r.orig.end-a;t.push(r)}for(var i in t){n.api.textTracks[0].addCue(t[i])}n.api.textTracks[0].currentOffset=a}n.api.setSubtitle=function(e){var t=r.getElementsByTagName("track");for(var n=t.length-1;n>=0;n--){r.removeChild(t[n])}if(e){var i=document.createElement("track");r.appendChild(i);i.kind="subtitles";i.label=e.label;i.srclang=e.lang;i.src=e.src;i.setAttribute("default","");i.onload=v}};MistUtil.event.addListener(r,"ended",function(){if(n.api.loop){n.webrtc.connect()}});if("decodingIssues"in e.skin.blueprints){var b=["nackCount","pliCount","packetsLost","packetsReceived","bytesReceived"];for(var m in b){n.api[b[m]]=0}var y=function(){e.timers.start(function(){n.webrtc.getStats(function(e){for(var t in e){for(var i in b){if(b[i]in e[t]){n.api[b[i]]=e[t][b[i]]}}break}});y()},1e3)};y()}n.api.unload=function(){try{n.webrtc.stop()}catch(e){}};t(r)};