mistplayers.videojs={name:"VideoJS player",mimes:["html5/application/vnd.apple.mpegurl"],priority:MistUtil.object.keys(mistplayers).length+1,isMimeSupported:function(e){return this.mimes.indexOf(e)==-1?false:true},isBrowserSupported:function(e,t,i){if(location.protocol!=MistUtil.http.url.split(t.url).protocol){i.log("HTTP/HTTPS mismatch for this source");return false}if(location.protocol=="file:"&&e=="html5/application/vnd.apple"){i.log("This source ("+e+") won't load if the page is run via file://");return false}return"MediaSource"in window},player:function(){},scriptsrc:function(e){return e+"/videojs.js"}};var p=mistplayers.videojs.player;p.prototype=new MistPlayer;p.prototype.build=function(e,t){var i=this;function r(){if(e.destroyed){return}e.log("Building VideoJS player..");var r=document.createElement("video");if(e.source.type!="html5/video/ogg"){r.crossOrigin="anonymous"}var o=e.source.type.split("/");o.shift();var n=document.createElement("source");n.setAttribute("src",e.source.url);i.source=n;r.appendChild(n);n.type=o.join("/");e.log("Adding "+n.type+" source @ "+e.source.url);if(n.type=="application/vnd.apple.mpegurl"){n.type="application/x-mpegURL"}MistUtil.class.add(r,"video-js");var s={};if(e.options.autoplay){s.autoplay=true}if(e.options.loop&&e.info.type!="live"){s.loop=true;r.loop=true}if(e.options.muted){s.muted=true;r.muted=true}if(e.options.poster){s.poster=e.options.poster}if(e.options.controls=="stock"){r.setAttribute("controls","");if(!document.getElementById("videojs-css")){var l=document.createElement("link");l.rel="stylesheet";l.href=e.options.host+"/skins/videojs.css";l.id="videojs-css";document.head.appendChild(l)}}i.onready(function(){i.videojs=videojs(r,s,function(){e.log("Videojs initialized")});i.api.unload=function(){videojs(r).dispose()}});e.log("Built html");if("Proxy"in window&&"Reflect"in window){var a={get:{},set:{}};e.player.api=new Proxy(r,{get:function(e,t,i){if(t in a.get){return a.get[t].apply(e,arguments)}var r=e[t];if(typeof r==="function"){return function(){return r.apply(e,arguments)}}return r},set:function(e,t,i){if(t in a.set){return a.set[t].call(e,i)}return e[t]=i}});if(e.info.type=="live"){function p(e){var t=0;if(e.buffered.length){t=e.buffered.end(e.buffered.length-1)}return t}var u=90;a.get.duration=function(){return(e.info.lastms+(new Date).getTime()-e.info.updated.getTime())*.001};e.player.api.lastProgress=new Date;e.player.api.liveOffset=0;MistUtil.event.addListener(r,"progress",function(){e.player.api.lastProgress=new Date});a.set.currentTime=function(t){var i=e.player.api.currentTime-t;var r=t-e.player.api.duration;e.log("Seeking to "+MistUtil.format.time(t)+" ("+Math.round(r*-10)/10+"s from live)");e.video.currentTime-=i};a.get.currentTime=function(){return this.currentTime+e.info.lastms*.001-e.player.api.liveOffset-u}}}else{i.api=r}e.player.setSize=function(t){if("videojs"in e.player){e.player.videojs.dimensions(t.width,t.height);r.parentNode.style.width=t.width+"px";r.parentNode.style.height=t.height+"px"}this.api.style.width=t.width+"px";this.api.style.height=t.height+"px"};e.player.api.setSource=function(t){if(!e.player.videojs){return}if(e.player.videojs.src()!=t){e.player.videojs.src({type:e.player.videojs.currentSource().type,src:t})}};e.player.api.setSubtitle=function(e){var t=r.getElementsByTagName("track");for(var i=t.length-1;i>=0;i--){r.removeChild(t[i])}if(e){var o=document.createElement("track");r.appendChild(o);o.kind="subtitles";o.label=e.label;o.srclang=e.lang;o.src=e.src;o.setAttribute("default","")}};t(r)}if("videojs"in window){r()}else{var o=MistUtil.scripts.insert(e.urlappend(mistplayers.videojs.scriptsrc(e.options.host)),{onerror:function(t){var i="Failed to load videojs.js";if(t.message){i+=": "+t.message}e.showError(i)},onload:r},e)}};