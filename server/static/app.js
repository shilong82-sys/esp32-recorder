/**
 * ESP32 AI Recorder — Web UI 前端交互逻辑 v0.5
 *
 * 功能：
 * - Tab 切换（4 tabs）
 * - 认证（自动登录 / 手动登录 / 退出）
 * - 录音列表分页 + 日期筛选 + 标签筛选
 * - 转写记录列表分页
 * - 转写结果展示（时间戳分段 + 说话人标识）
 * - 转写编辑（textarea + 保存）
 * - 音频播放（<audio> 标签 + /stream 端点）
 * - 全局搜索（debounce 300ms）
 * - 设置 Tab（加载/保存设置，模型列表，说话人分离，清理天数）
 * - 系统状态自动刷新
 * - 删除确认对话框
 * - 复制转写文本
 * - 手动触发转写（支持模型选择）
 * - 批量操作（批量删除/转写）
 * - 标签系统（添加/移除标签，标签筛选）
 * - SRT/VTT 导出
 * - 拖拽上传
 * - 暗色模式
 * - 数据备份（导出/导入）
 */

(function () {
  "use strict";

  // ================================================================
  // 常量
  // ================================================================
  var PAGE_SIZE = 20;
  var STATUS_REFRESH_INTERVAL = 10000;
  var PROCESSING_CHECK_INTERVAL = 5000;
  var SEARCH_DEBOUNCE_MS = 300;
  var TAG_COLORS = [
    "#6366f1", "#8b5cf6", "#ec4899", "#ef4444",
    "#f97316", "#eab308", "#22c55e", "#14b8a6",
    "#3b82f6", "#6b7280"
  ];
  var SPEAKER_COLORS = ["#3b82f6", "#22c55e", "#f97316", "#ec4899", "#8b5cf6", "#14b8a6"];

  // ================================================================
  // 状态
  // ================================================================
  var state = {
    filesPage: 1,
    filesTotal: 0,
    transcriptsPage: 1,
    transcriptsTotal: 0,
    selectedFileId: null,
    selectedTranscriptFileId: null,
    deleteFileId: null,
    statusTimer: null,
    processingCheckTimer: null,
    searchQuery: "",
    searchTimer: null,
    dateFrom: "",
    dateTo: "",
    isAuthenticated: false,
    editMode: false,
    currentTranscriptText: "",
    selectedFileIds: new Set(),
    allTags: [],
    triggerTranscribeFileId: null,
  };

  // ================================================================
  // 工具函数
  // ================================================================

  function formatSize(bytes) {
    if (bytes < 1024) return bytes + " B";
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB";
    return (bytes / (1024 * 1024)).toFixed(1) + " MB";
  }

  function formatDateTime(dtStr) {
    if (!dtStr) return "—";
    var d = new Date(dtStr);
    var y = d.getFullYear();
    var m = String(d.getMonth() + 1).padStart(2, "0");
    var day = String(d.getDate()).padStart(2, "0");
    var h = String(d.getHours()).padStart(2, "0");
    var min = String(d.getMinutes()).padStart(2, "0");
    var s = String(d.getSeconds()).padStart(2, "0");
    return y + "-" + m + "-" + day + " " + h + ":" + min + ":" + s;
  }

  function formatDuration(seconds) {
    if (seconds == null || seconds === 0) return "—";
    var totalSec = Math.floor(seconds);
    var m = Math.floor(totalSec / 60);
    var s = totalSec % 60;
    if (m > 0) {
      return m + ":" + String(s).padStart(2, "0");
    }
    return s + "s";
  }

  function formatTimestamp(seconds) {
    if (seconds == null) return "00:00";
    var totalSec = Math.floor(seconds);
    var m = Math.floor(totalSec / 60);
    var s = totalSec % 60;
    return String(m).padStart(2, "0") + ":" + String(s).padStart(2, "0");
  }

  function formatUptime(startTimestamp) {
    if (!startTimestamp) return "—";
    var now = Date.now() / 1000;
    var diff = Math.floor(now - startTimestamp);
    var d = Math.floor(diff / 86400);
    var h = Math.floor((diff % 86400) / 3600);
    var m = Math.floor((diff % 3600) / 60);
    var parts = [];
    if (d > 0) parts.push(d + "d");
    if (h > 0) parts.push(h + "h");
    parts.push(m + "m");
    return parts.join(" ");
  }

  function statusBadge(status) {
    var icons = {
      completed: "✅", processing: "⏳", pending: "🕒", failed: "❌"
    };
    var labels = {
      completed: "已完成", processing: "转写中", pending: "等待中", failed: "失败"
    };
    var icon = icons[status] || "❓";
    var label = labels[status] || status;
    return '<span class="status-badge ' + status + '">' + icon + " " + label + "</span>";
  }

  function showToast(message, type) {
    type = type || "info";
    var container = document.getElementById("toast-container");
    var el = document.createElement("div");
    el.className = "toast " + type;
    el.textContent = message;
    container.appendChild(el);
    setTimeout(function () {
      el.style.opacity = "0";
      el.style.transition = "opacity 0.3s";
      setTimeout(function () {
        if (el.parentNode) el.parentNode.removeChild(el);
      }, 300);
    }, 3000);
  }

  function apiGet(url) {
    return fetch(url).then(function (res) {
      if (res.status === 401) { showLoginOverlay(); throw new Error("Unauthorized"); }
      if (!res.ok) throw new Error("HTTP " + res.status);
      return res.json();
    });
  }

  function apiDelete(url) {
    return fetch(url, { method: "DELETE" }).then(function (res) {
      if (!res.ok) throw new Error("HTTP " + res.status);
      return res.json();
    });
  }

  function apiPost(url, body) {
    var opts = { method: "POST" };
    if (body) {
      opts.headers = { "Content-Type": "application/json" };
      opts.body = JSON.stringify(body);
    }
    return fetch(url, opts).then(function (res) {
      if (!res.ok) throw new Error("HTTP " + res.status);
      return res.json();
    });
  }

  function apiPut(url, body) {
    return fetch(url, {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    }).then(function (res) {
      if (!res.ok) throw new Error("HTTP " + res.status);
      return res.json();
    });
  }

  function escapeHtml(str) {
    if (!str) return "";
    return str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;").replace(/'/g, "&#039;");
  }

  function truncate(str, maxLen) {
    if (!str) return "";
    if (str.length <= maxLen) return str;
    return str.substring(0, maxLen - 3) + "...";
  }

  function hexToRgba(hex, alpha) {
    var r = parseInt(hex.slice(1, 3), 16);
    var g = parseInt(hex.slice(3, 5), 16);
    var b = parseInt(hex.slice(5, 7), 16);
    return "rgba(" + r + "," + g + "," + b + "," + alpha + ")";
  }

  // ================================================================
  // 认证模块
  // ================================================================

  function showLoginOverlay() {
    state.isAuthenticated = false;
    var overlay = document.getElementById("login-overlay");
    if (overlay) overlay.style.display = "flex";
  }

  function hideLoginOverlay() {
    state.isAuthenticated = true;
    var overlay = document.getElementById("login-overlay");
    if (overlay) overlay.style.display = "none";
  }

  function checkAuth() {
    var savedPassword = localStorage.getItem("rec_password");
    if (savedPassword) {
      doLogin(savedPassword, true);
    } else {
      showLoginOverlay();
    }
  }

  function doLogin(password, isAuto) {
    apiPost("/api/auth/login", { password: password })
      .then(function (resp) {
        if (resp.code === 0) {
          hideLoginOverlay();
          localStorage.setItem("rec_password", password);
          if (!isAuto) showToast("登录成功", "success");
        } else {
          if (isAuto) { localStorage.removeItem("rec_password"); showLoginOverlay(); }
          else {
            var errorEl = document.getElementById("login-error");
            errorEl.textContent = "密码错误"; errorEl.classList.remove("hidden");
          }
        }
      })
      .catch(function () {
        if (isAuto) showLoginOverlay(); else showToast("登录失败", "error");
      });
  }

  function doLogout() {
    apiPost("/api/auth/logout").then(function () {
      localStorage.removeItem("rec_password"); showLoginOverlay(); showToast("已退出登录", "info");
    }).catch(function () { localStorage.removeItem("rec_password"); showLoginOverlay(); });
  }

  // ================================================================
  // 暗色模式
  // ================================================================

  function initTheme() {
    var saved = localStorage.getItem("rec_theme");
    if (saved === "dark") {
      document.documentElement.setAttribute("data-theme", "dark");
      document.getElementById("btn-theme-toggle").textContent = "☀️";
    }
  }

  function toggleTheme() {
    var current = document.documentElement.getAttribute("data-theme");
    var btn = document.getElementById("btn-theme-toggle");
    if (current === "dark") {
      document.documentElement.setAttribute("data-theme", "light");
      btn.textContent = "🌙";
      localStorage.setItem("rec_theme", "light");
    } else {
      document.documentElement.setAttribute("data-theme", "dark");
      btn.textContent = "☀️";
      localStorage.setItem("rec_theme", "dark");
    }
  }

  // ================================================================
  // Tab 切换
  // ================================================================

  function initTabs() {
    var btns = document.querySelectorAll(".tab-btn");
    btns.forEach(function (btn) {
      btn.addEventListener("click", function () {
        var tab = btn.getAttribute("data-tab");
        btns.forEach(function (b) { b.classList.remove("active"); });
        btn.classList.add("active");
        document.querySelectorAll(".tab-panel").forEach(function (panel) { panel.classList.remove("active"); });
        var target = document.getElementById("tab-" + tab);
        if (target) target.classList.add("active");
        if (tab === "files") loadFiles();
        else if (tab === "transcripts") loadTranscripts();
        else if (tab === "settings") loadSettings();
        else if (tab === "status") loadStatus();
      });
    });
  }

  // ================================================================
  // 标签管理
  // ================================================================

  function loadTags() {
    apiGet("/api/tags").then(function (resp) {
      if (resp.code === 0 && resp.data) {
        state.allTags = resp.data;
        renderTagFilter();
      }
    }).catch(function () {});
  }

  function renderTagFilter() {
    var select = document.getElementById("tag-filter");
    if (!select) return;
    var val = select.value;
    select.innerHTML = '<option value="">全部标签</option>';
    state.allTags.forEach(function (tag) {
      var opt = document.createElement("option");
      opt.value = tag.id;
      opt.textContent = tag.name;
      select.appendChild(opt);
    });
    select.value = val;
  }

  function renderDetailTags(tags, fileId) {
    var area = document.getElementById("detail-tags-area");
    var list = document.getElementById("detail-tags-list");
    if (!area || !list) return;

    area.classList.remove("hidden");
    list.innerHTML = "";

    if (tags && tags.length > 0) {
      tags.forEach(function (tag) {
        var badge = document.createElement("span");
        badge.className = "tag-badge";
        badge.style.backgroundColor = hexToRgba(tag.color, 0.15);
        badge.style.color = tag.color;
        badge.innerHTML = escapeHtml(tag.name) + ' <span class="tag-remove" data-tag-id="' + tag.id + '" title="移除标签">✕</span>';
        list.appendChild(badge);
      });
    }

    // 移除标签事件
    list.querySelectorAll(".tag-remove").forEach(function (el) {
      el.addEventListener("click", function () {
        var tagId = parseInt(el.getAttribute("data-tag-id"));
        apiPost("/api/files/" + fileId + "/tags", { tag_ids: [tagId] }).then(function () {
          return fetch("/api/files/" + fileId + "/tags", {
            method: "DELETE", headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ tag_ids: [tagId] })
          });
        }).then(function (r) { return r.json(); }).then(function () {
          showToast("标签已移除", "success");
          viewFile(fileId);
          loadTags();
        }).catch(function (err) { showToast("移除标签失败: " + err.message, "error"); });
      });
    });
  }

  function initTagInput(fileId) {
    var input = document.getElementById("tag-input");
    var suggestions = document.getElementById("tag-suggestions");
    if (!input) return;

    input.value = "";
    input.oninput = function () {
      var val = input.value.trim().toLowerCase();
      if (!val) { suggestions.classList.add("hidden"); return; }
      var matches = state.allTags.filter(function (t) {
        return t.name.toLowerCase().indexOf(val) >= 0;
      });
      if (matches.length === 0) {
        suggestions.innerHTML = '<div class="tag-suggestion-item" data-action="create">创建 "' + escapeHtml(input.value.trim()) + '"</div>';
      } else {
        suggestions.innerHTML = matches.map(function (t) {
          return '<div class="tag-suggestion-item" data-tag-id="' + t.id + '">' + escapeHtml(t.name) + '</div>';
        }).join("");
      }
      suggestions.classList.remove("hidden");
    };

    input.onkeydown = function (e) {
      if (e.key === "Enter") {
        e.preventDefault();
        var val = input.value.trim();
        if (!val) return;
        // Try to find existing tag
        var existing = state.allTags.find(function (t) { return t.name.toLowerCase() === val.toLowerCase(); });
        if (existing) {
          addTagToFile(fileId, existing.id);
        } else {
          // Create new tag first
          apiPost("/api/tags", { name: val }).then(function (resp) {
            if (resp.code === 0 && resp.data) {
              addTagToFile(fileId, resp.data.id);
              loadTags();
            } else {
              showToast("创建标签失败: " + resp.message, "error");
            }
          }).catch(function (err) { showToast("创建标签失败: " + err.message, "error"); });
        }
        suggestions.classList.add("hidden");
        input.value = "";
      }
    };

    suggestions.onclick = function (e) {
      var item = e.target.closest(".tag-suggestion-item");
      if (!item) return;
      var tagId = item.getAttribute("data-tag-id");
      var action = item.getAttribute("data-action");
      if (tagId) {
        addTagToFile(fileId, parseInt(tagId));
      } else if (action === "create") {
        var name = input.value.trim();
        apiPost("/api/tags", { name: name }).then(function (resp) {
          if (resp.code === 0 && resp.data) {
            addTagToFile(fileId, resp.data.id);
            loadTags();
          }
        }).catch(function (err) { showToast("创建标签失败: " + err.message, "error"); });
      }
      suggestions.classList.add("hidden");
      input.value = "";
    };
  }

  function addTagToFile(fileId, tagId) {
    apiPost("/api/files/" + fileId + "/tags", { tag_ids: [tagId] })
      .then(function (resp) {
        if (resp.code === 0) {
          showToast("标签已添加", "success");
          viewFile(fileId);
        } else {
          showToast("添加标签失败: " + resp.message, "error");
        }
      }).catch(function (err) { showToast("添加标签失败: " + err.message, "error"); });
  }

  // ================================================================
  // 录音列表
  // ================================================================

  function loadFiles() {
    var url = "/api/files?page=" + state.filesPage + "&page_size=" + PAGE_SIZE + "&sort=upload_time&order=desc";

    var dateFrom = document.getElementById("date-from").value;
    var dateTo = document.getElementById("date-to").value;
    if (dateFrom) url += "&date_from=" + encodeURIComponent(dateFrom);
    if (dateTo) url += "&date_to=" + encodeURIComponent(dateTo);

    var tagFilter = document.getElementById("tag-filter");
    if (tagFilter && tagFilter.value) url += "&tag_id=" + tagFilter.value;

    apiGet(url).then(function (resp) {
      if (resp.code !== 0) { showToast("加载文件列表失败: " + resp.message, "error"); return; }
      var data = resp.data;
      state.filesTotal = data.total;
      renderFilesTable(data.items);
      renderPagination("files-pagination", data.page, Math.ceil(data.total / data.page_size), function (page) {
        state.filesPage = page; loadFiles();
      });
    }).catch(function (err) {
      if (err.message !== "Unauthorized") showToast("网络错误: " + err.message, "error");
    });
  }

  function renderFilesTable(items) {
    var tbody = document.getElementById("files-tbody");
    if (!items || items.length === 0) {
      tbody.innerHTML = '<tr><td colspan="7" class="empty-msg">暂无录音文件</td></tr>';
      return;
    }

    var html = "";
    items.forEach(function (item) {
      var transStatus = item.transcription ? item.transcription.status : "pending";
      var isSelected = item.id === state.selectedFileId;
      var isChecked = state.selectedFileIds.has(item.id);

      html += '<tr class="' + (isSelected ? "selected" : "") + '" data-file-id="' + item.id + '">';
      html += '<td class="td-checkbox"><input type="checkbox" class="file-checkbox" data-file-id="' + item.id + '"' + (isChecked ? " checked" : "") + '></td>';
      html += '<td class="text-mono" title="' + escapeHtml(item.filename) + '">' + escapeHtml(truncate(item.filename, 30));
      // Show tags inline
      if (item.tags && item.tags.length > 0) {
        html += ' <span class="file-tags-inline">';
        item.tags.forEach(function (tag) {
          html += '<span class="tag-badge-inline" style="background:' + hexToRgba(tag.color, 0.15) + ';color:' + tag.color + ';">' + escapeHtml(tag.name) + '</span>';
        });
        html += '</span>';
      }
      html += "</td>";
      html += "<td>" + formatDuration(item.duration) + "</td>";
      html += "<td>" + formatSize(item.file_size) + "</td>";
      html += "<td>" + formatDateTime(item.upload_time) + "</td>";
      html += "<td>" + statusBadge(transStatus) + "</td>";
      html += "<td>";
      html += '<button class="btn-icon" title="播放" onclick="window._playFile(' + item.id + ')">▶</button> ';
      html += '<button class="btn-icon" title="查看详情" onclick="window._viewFile(' + item.id + ')">👁</button> ';
      if (transStatus === "completed" || transStatus === "failed") {
        html += '<button class="btn-icon" title="重新转写" onclick="window._triggerTranscribe(' + item.id + ')">🔄</button> ';
      }
      html += '<button class="btn-icon danger" title="删除" onclick="window._confirmDelete(' + item.id + ", '" + escapeHtml(item.filename).replace(/'/g, "\\'") + "'" + ')">🗑</button>';
      html += "</td>";
      html += "</tr>";
    });
    tbody.innerHTML = html;

    // Checkbox events
    tbody.querySelectorAll(".file-checkbox").forEach(function (cb) {
      cb.addEventListener("change", function (e) {
        var fid = parseInt(cb.getAttribute("data-file-id"));
        if (cb.checked) state.selectedFileIds.add(fid);
        else state.selectedFileIds.delete(fid);
        updateBatchBar();
      });
    });
  }

  function updateBatchBar() {
    var bar = document.getElementById("batch-bar");
    var count = document.getElementById("batch-count");
    if (state.selectedFileIds.size > 0) {
      bar.classList.remove("hidden");
      count.textContent = "已选 " + state.selectedFileIds.size + " 项";
    } else {
      bar.classList.add("hidden");
    }
  }

  // ================================================================
  // 转写详情 + 时间戳分段 + 说话人
  // ================================================================

  function viewFile(fileId) {
    state.selectedFileId = fileId;

    var rows = document.querySelectorAll("#files-tbody tr");
    rows.forEach(function (r) {
      r.classList.remove("selected");
      if (r.getAttribute("data-file-id") === String(fileId)) r.classList.add("selected");
    });

    var panel = document.getElementById("file-detail-panel");
    panel.classList.remove("hidden");

    var filenameEl = document.getElementById("detail-filename");
    var metaEl = document.getElementById("detail-meta");
    var contentEl = document.getElementById("detail-content");
    var actionsEl = document.getElementById("detail-actions");

    contentEl.innerHTML = '<p class="empty-msg">加载中...</p>';
    actionsEl.classList.add("hidden");

    Promise.all([
      apiGet("/api/files/" + fileId),
      apiGet("/api/transcripts/" + fileId),
    ]).then(function (results) {
      var fileResp = results[0];
      var transResp = results[1];

      if (fileResp.code !== 0) {
        contentEl.innerHTML = '<p class="empty-msg">加载文件信息失败</p>';
        return;
      }

      var file = fileResp.data;
      filenameEl.textContent = file.filename;
      var metaParts = [];
      if (file.duration) metaParts.push(formatDuration(file.duration));
      metaParts.push(formatSize(file.file_size));
      metaParts.push(formatDateTime(file.upload_time));
      metaEl.textContent = metaParts.join(" | ");

      // 标签
      renderDetailTags(file.tags, fileId);
      initTagInput(fileId);

      if (transResp.code !== 0 || !transResp.data) {
        contentEl.innerHTML = '<p class="empty-msg">暂无转写记录</p>';
        return;
      }

      var trans = transResp.data;
      var segments = null;
      var speakers = null;

      if (trans.segments) {
        try { segments = JSON.parse(trans.segments); } catch (e) { segments = null; }
      }
      if (trans.speakers) {
        try { speakers = JSON.parse(trans.speakers); } catch (e) { speakers = null; }
      }

      if (trans.status === "completed" && trans.text) {
        // Render segments view
        if (segments && segments.length > 0) {
          renderSegmentsView(contentEl, segments, speakers);
        } else {
          contentEl.textContent = trans.text;
        }
        actionsEl.classList.remove("hidden");

        // 下载按钮
        document.getElementById("btn-download-txt").onclick = function () {
          window.open("/api/transcripts/" + fileId + "/export?format=txt", "_blank");
        };
        document.getElementById("btn-export-srt").onclick = function () {
          window.open("/api/transcripts/" + fileId + "/export?format=srt", "_blank");
        };
        document.getElementById("btn-export-vtt").onclick = function () {
          window.open("/api/transcripts/" + fileId + "/export?format=vtt", "_blank");
        };
        document.getElementById("btn-copy-txt").onclick = function () {
          copyToClipboard(trans.text);
        };
      } else if (trans.status === "failed") {
        contentEl.innerHTML = '<p class="empty-msg" style="color:var(--color-danger)">转写失败: ' + escapeHtml(trans.error_msg || "未知错误") + "</p>";
      } else if (trans.status === "processing") {
        contentEl.innerHTML = '<p class="empty-msg">⏳ 正在转写中，请稍后刷新查看...</p>';
      } else {
        contentEl.innerHTML = '<p class="empty-msg">等待转写中...</p>';
      }
    }).catch(function (err) {
      contentEl.innerHTML = '<p class="empty-msg">加载失败: ' + escapeHtml(err.message) + "</p>";
    });
  }

  function renderSegmentsView(container, segments, speakers) {
    // Build speaker map: segment_index -> { id, name }
    var speakerMap = {};
    if (speakers) {
      speakers.forEach(function (sp) {
        var name = sp.name || sp.id;
        sp.segment_indices.forEach(function (idx) {
          speakerMap[idx] = { id: sp.id, name: name };
        });
      });
    }

    var html = '<div class="segments-list">';
    segments.forEach(function (seg, idx) {
      var time = formatTimestamp(seg.start);
      var text = escapeHtml(seg.text || "");
      var speakerHtml = "";

      if (speakerMap[idx]) {
        var sp = speakerMap[idx];
        var colorIdx = parseInt(sp.id.replace("S", "")) - 1;
        var color = SPEAKER_COLORS[colorIdx % SPEAKER_COLORS.length];
        speakerHtml = '<span class="speaker-badge" style="background:' + hexToRgba(color, 0.15) + ';color:' + color + ';" data-speaker-id="' + escapeHtml(sp.id) + '" data-file-id="' + state.selectedFileId + '">' + escapeHtml(sp.name) + '</span> ';
      }

      html += '<div class="segment-item">';
      html += '<span class="segment-time" data-start="' + seg.start + '">[' + time + ']</span>';
      html += speakerHtml;
      html += '<span class="segment-text">' + text + '</span>';
      html += '</div>';
    });
    html += '</div>';

    container.innerHTML = html;

    // Click timestamp to seek audio
    container.querySelectorAll(".segment-time").forEach(function (el) {
      el.addEventListener("click", function () {
        var start = parseFloat(el.getAttribute("data-start"));
        var audio = document.getElementById("audio-player");
        if (audio && audio.src) {
          audio.currentTime = start;
          audio.play().catch(function () {});
        }
      });
    });

    // Double click speaker badge to edit name
    container.querySelectorAll(".speaker-badge").forEach(function (el) {
      el.addEventListener("dblclick", function () {
        var speakerId = el.getAttribute("data-speaker-id");
        var fileId = parseInt(el.getAttribute("data-file-id"));
        var currentName = el.textContent.trim();
        var newName = prompt("修改说话人名称:", currentName);
        if (newName && newName !== currentName) {
          // Fetch current speakers and update
          apiGet("/api/transcripts/" + fileId + "/speakers").then(function (resp) {
            if (resp.code === 0 && resp.data.speakers) {
              var spks = resp.data.speakers;
              spks.forEach(function (s) { if (s.id === speakerId) s.name = newName; });
              apiPut("/api/transcripts/" + fileId + "/speakers", { speakers: spks })
                .then(function () { showToast("说话人名称已更新", "success"); viewFile(fileId); })
                .catch(function (err) { showToast("更新失败: " + err.message, "error"); });
            }
          });
        }
      });
      el.title = "双击编辑名称";
    });
  }

  // ================================================================
  // 音频播放模块
  // ================================================================

  function playFile(fileId) {
    var audioPlayer = document.getElementById("audio-player");
    var playerWrap = document.getElementById("audio-player-wrap");
    var playerName = document.getElementById("audio-player-name");

    audioPlayer.src = "/api/files/" + fileId + "/stream";
    audioPlayer.load();
    playerWrap.classList.remove("hidden");
    playerName.textContent = "加载中...";

    apiGet("/api/files/" + fileId).then(function (resp) {
      if (resp.code === 0 && resp.data) playerName.textContent = resp.data.filename;
    }).catch(function () {});

    audioPlayer.play().catch(function () {});
  }

  // ================================================================
  // 转写记录
  // ================================================================

  function loadTranscripts() {
    var statusFilter = document.getElementById("transcript-status-filter").value;
    var url = "/api/transcripts?page=" + state.transcriptsPage + "&page_size=" + PAGE_SIZE;
    if (statusFilter) url += "&status=" + encodeURIComponent(statusFilter);

    apiGet(url).then(function (resp) {
      if (resp.code !== 0) { showToast("加载转写记录失败: " + resp.message, "error"); return; }
      var data = resp.data;
      state.transcriptsTotal = data.total;
      renderTranscriptsTable(data.items);
      renderPagination("transcripts-pagination", data.page, Math.ceil(data.total / data.page_size), function (page) {
        state.transcriptsPage = page; loadTranscripts();
      });
    }).catch(function (err) { showToast("网络错误: " + err.message, "error"); });
  }

  function renderTranscriptsTable(items) {
    var tbody = document.getElementById("transcripts-tbody");
    if (!items || items.length === 0) {
      tbody.innerHTML = '<tr><td colspan="6" class="empty-msg">暂无转写记录</td></tr>';
      return;
    }

    var html = "";
    items.forEach(function (item) {
      html += '<tr data-file-id="' + item.file_id + '" data-trans-id="' + item.id + '">';
      html += '<td><a href="javascript:void(0)" onclick="window._viewTranscript(' + item.file_id + ')">#' + item.file_id + "</a></td>";
      html += "<td>" + statusBadge(item.status) + "</td>";
      html += "<td>" + (item.language || "—") + "</td>";
      html += "<td>" + formatDuration(item.duration) + "</td>";
      html += "<td>" + formatDateTime(item.completed_at) + "</td>";
      html += "<td>";
      html += '<button class="btn-icon" title="查看详情" onclick="window._viewTranscript(' + item.file_id + ')">👁</button> ';
      if (item.status === "completed" || item.status === "failed") {
        html += '<button class="btn-icon" title="重新转写" onclick="window._triggerTranscribe(' + item.file_id + ')">🔄</button>';
      }
      html += "</td>";
      html += "</tr>";
    });
    tbody.innerHTML = html;
  }

  function viewTranscript(fileId) {
    state.selectedTranscriptFileId = fileId;
    state.editMode = false;

    var panel = document.getElementById("transcript-detail-panel");
    panel.classList.remove("hidden");

    var titleEl = document.getElementById("transcript-detail-title");
    var contentEl = document.getElementById("transcript-detail-content");
    var actionsEl = document.getElementById("transcript-detail-actions");
    var editBadge = document.getElementById("transcript-edited-badge");
    var editedAtEl = document.getElementById("transcript-edited-at");
    var btnEdit = document.getElementById("btn-edit-transcript");
    var btnSave = document.getElementById("btn-save-transcript");

    contentEl.innerHTML = '<p class="empty-msg">加载中...</p>';
    actionsEl.classList.add("hidden");
    editBadge.classList.add("hidden");
    editedAtEl.textContent = "";
    btnSave.classList.add("hidden");
    btnEdit.style.display = "";

    Promise.all([
      apiGet("/api/files/" + fileId),
      apiGet("/api/transcripts/" + fileId),
    ]).then(function (results) {
      var fileResp = results[0];
      var transResp = results[1];

      var filename = "File #" + fileId;
      if (fileResp.code === 0 && fileResp.data) filename = fileResp.data.filename;
      titleEl.textContent = filename;

      if (transResp.code !== 0 || !transResp.data) {
        contentEl.innerHTML = '<p class="empty-msg">暂无转写记录</p>';
        return;
      }

      var trans = transResp.data;
      state.currentTranscriptText = trans.text || "";

      if (trans.is_edited === 1) {
        editBadge.classList.remove("hidden");
        if (trans.edited_at) editedAtEl.textContent = "编辑于 " + formatDateTime(trans.edited_at);
      }

      if (trans.status === "completed" && trans.text) {
        var segments = null;
        var speakers = null;
        if (trans.segments) { try { segments = JSON.parse(trans.segments); } catch (e) {} }
        if (trans.speakers) { try { speakers = JSON.parse(trans.speakers); } catch (e) {} }

        if (segments && segments.length > 0) {
          renderSegmentsView(contentEl, segments, speakers);
          actionsEl.classList.remove("hidden");
        } else {
          var textarea = document.createElement("textarea");
          textarea.id = "transcript-text-editor";
          textarea.className = "transcript-editor";
          textarea.value = trans.text;
          textarea.readOnly = true;
          contentEl.innerHTML = "";
          contentEl.appendChild(textarea);
          actionsEl.classList.remove("hidden");
        }

        document.getElementById("btn-download-transcript-txt").onclick = function () {
          window.open("/api/transcripts/" + fileId + "/export?format=txt", "_blank");
        };
        document.getElementById("btn-export-transcript-srt").onclick = function () {
          window.open("/api/transcripts/" + fileId + "/export?format=srt", "_blank");
        };
        document.getElementById("btn-export-transcript-vtt").onclick = function () {
          window.open("/api/transcripts/" + fileId + "/export?format=vtt", "_blank");
        };
        document.getElementById("btn-copy-transcript-txt").onclick = function () {
          var editor = document.getElementById("transcript-text-editor");
          copyToClipboard(editor ? editor.value : trans.text);
        };

        btnEdit.onclick = function () {
          state.editMode = true;
          var editor = document.getElementById("transcript-text-editor");
          if (editor) { editor.readOnly = false; editor.focus(); }
          btnEdit.style.display = "none";
          btnSave.classList.remove("hidden");
        };

        btnSave.onclick = function () {
          var editor = document.getElementById("transcript-text-editor");
          if (editor) saveTranscript(fileId, editor.value);
        };

      } else if (trans.status === "failed") {
        contentEl.innerHTML = '<p class="empty-msg" style="color:var(--color-danger)">转写失败: ' + escapeHtml(trans.error_msg || "未知错误") + "</p>";
      } else if (trans.status === "processing") {
        contentEl.innerHTML = '<p class="empty-msg">⏳ 正在转写中...</p>';
      } else {
        contentEl.innerHTML = '<p class="empty-msg">等待转写中...</p>';
      }
    }).catch(function (err) {
      contentEl.innerHTML = '<p class="empty-msg">加载失败: ' + escapeHtml(err.message) + "</p>";
    });
  }

  function saveTranscript(fileId, text) {
    apiPut("/api/transcripts/" + fileId, { text: text })
      .then(function (resp) {
        if (resp.code === 0) { showToast("保存成功", "success"); state.editMode = false; viewTranscript(fileId); }
        else showToast("保存失败: " + resp.message, "error");
      })
      .catch(function (err) { showToast("保存失败: " + err.message, "error"); });
  }

  // ================================================================
  // 设置 Tab
  // ================================================================

  function loadSettings() {
    Promise.all([
      apiGet("/api/settings"),
      apiGet("/api/settings/models"),
    ]).then(function (results) {
      var settingsResp = results[0];
      var modelsResp = results[1];

      if (settingsResp.code !== 0) return;
      var settings = settingsResp.data;

      var langSelect = document.getElementById("setting-language");
      if (settings.transcribe_language) langSelect.value = settings.transcribe_language;

      var modelSelect = document.getElementById("setting-model");
      if (modelsResp.code === 0 && modelsResp.data) {
        modelSelect.innerHTML = "";
        modelsResp.data.forEach(function (model) {
          var opt = document.createElement("option");
          opt.value = model;
          opt.textContent = model.split("/").pop();
          modelSelect.appendChild(opt);
        });
        if (settings.transcribe_model) modelSelect.value = settings.transcribe_model;
      }

      var autoToggle = document.getElementById("setting-auto-transcribe");
      autoToggle.checked = settings.auto_transcribe !== "false";

      // 说话人分离
      var diarizeToggle = document.getElementById("setting-diarize");
      var diarizeHint = document.getElementById("diarize-hint");
      diarizeToggle.checked = settings.diarize_enabled === "true";
      if (settings.diarizer_available === "false") {
        diarizeToggle.disabled = true;
        diarizeHint.textContent = "(pyannote-audio 未安装)";
      } else {
        diarizeToggle.disabled = false;
        diarizeHint.textContent = "";
      }

      // 清理天数
      var cleanupInput = document.getElementById("setting-cleanup-days");
      if (settings.cleanup_days) cleanupInput.value = settings.cleanup_days;
    }).catch(function () {});
  }

  function saveSettings() {
    var data = {
      transcribe_language: document.getElementById("setting-language").value,
      transcribe_model: document.getElementById("setting-model").value,
      auto_transcribe: document.getElementById("setting-auto-transcribe").checked ? "true" : "false",
      diarize_enabled: document.getElementById("setting-diarize").checked ? "true" : "false",
      cleanup_days: document.getElementById("setting-cleanup-days").value || "90",
    };

    apiPut("/api/settings", data)
      .then(function (resp) {
        if (resp.code === 0) showToast("设置已保存", "success");
        else showToast("保存失败: " + resp.message, "error");
      })
      .catch(function (err) { showToast("保存失败: " + err.message, "error"); });
  }

  // ================================================================
  // 搜索模块
  // ================================================================

  function doSearch(keyword) {
    if (!keyword || !keyword.trim()) {
      document.getElementById("search-results").classList.add("hidden");
      return;
    }

    apiGet("/api/search?q=" + encodeURIComponent(keyword.trim()) + "&limit=20")
      .then(function (resp) {
        if (resp.code !== 0) return;
        var items = resp.data;
        var resultsEl = document.getElementById("search-results");
        var listEl = document.getElementById("search-results-list");
        var titleEl = document.getElementById("search-results-title");

        if (!items || items.length === 0) {
          titleEl.textContent = "搜索结果（0 条）";
          listEl.innerHTML = '<p class="empty-msg">未找到匹配的转写内容</p>';
        } else {
          titleEl.textContent = "搜索结果（" + items.length + " 条）";
          var html = "";
          items.forEach(function (item) {
            var snippet = escapeHtml(item.snippet || "").replace(/\*\*/g, function () { return "§§MARK§§"; });
            snippet = snippet.replace(/§§MARK§§(.*?)§§MARK§§/g, "<mark>$1</mark>");
            html += '<div class="search-result-item" onclick="window._viewFile(' + item.file_id + ')">';
            html += '<div class="search-result-header">';
            html += '<span class="search-result-filename">' + escapeHtml(item.filename) + '</span>';
            html += '<span class="search-result-meta">';
            if (item.duration) html += formatDuration(item.duration) + ' | ';
            html += formatDateTime(item.upload_time) + '</span>';
            html += '</div>';
            html += '<div class="search-result-snippet">' + snippet + '</div>';
            html += '</div>';
          });
          listEl.innerHTML = html;
        }
        resultsEl.classList.remove("hidden");
      })
      .catch(function (err) {
        if (err.message !== "Unauthorized") showToast("搜索失败: " + err.message, "error");
      });
  }

  function initSearch() {
    var input = document.getElementById("search-input");
    input.addEventListener("input", function () {
      var val = input.value;
      if (state.searchTimer) clearTimeout(state.searchTimer);
      state.searchTimer = setTimeout(function () { doSearch(val); }, SEARCH_DEBOUNCE_MS);
    });
  }

  // ================================================================
  // 系统状态
  // ================================================================

  function loadStatus() {
    apiGet("/api/status").then(function (resp) {
      if (resp.code !== 0) return;
      var data = resp.data;

      var diskUsed = data.disk_used_bytes;
      var diskTotal = data.disk_total_bytes;
      var diskPercent = diskTotal > 0 ? ((diskUsed / diskTotal) * 100).toFixed(1) : 0;
      document.getElementById("stat-disk").textContent = formatSize(diskUsed) + " / " + formatSize(diskTotal);
      document.getElementById("stat-disk-bar").style.width = diskPercent + "%";

      document.getElementById("stat-files").textContent = data.file_count;

      var stats = data.transcription_stats;
      document.getElementById("stat-transcripts").textContent = stats.completed + " 完成";
      document.getElementById("stat-transcripts-detail").textContent =
        "等待: " + stats.pending + " | 处理中: " + stats.processing + " | 失败: " + stats.failed + " | 总计: " + stats.total;

      var whisperEl = document.getElementById("stat-whisper");
      if (stats.processing > 0) { whisperEl.textContent = "🔄 转写中"; whisperEl.style.color = "var(--color-warning)"; }
      else if (stats.pending > 0) { whisperEl.textContent = "⏳ 待处理"; whisperEl.style.color = "var(--color-info)"; }
      else { whisperEl.textContent = "✅ 就绪"; whisperEl.style.color = "var(--color-success)"; }

      document.getElementById("stat-uptime").textContent = formatUptime(window.SERVER_START_TS);
    }).catch(function () {});
  }

  function startStatusAutoRefresh() {
    if (state.statusTimer) clearInterval(state.statusTimer);
    state.statusTimer = setInterval(function () {
      var statusPanel = document.getElementById("tab-status");
      if (statusPanel && statusPanel.classList.contains("active")) loadStatus();
    }, STATUS_REFRESH_INTERVAL);
  }

  function startProcessingCheck() {
    if (state.processingCheckTimer) clearInterval(state.processingCheckTimer);
    state.processingCheckTimer = setInterval(function () {
      apiGet("/api/status").then(function (resp) {
        if (resp.code !== 0) return;
        var stats = resp.data.transcription_stats;
        var hasActive = stats.processing > 0 || stats.pending > 0;
        if (hasActive) {
          var filesPanel = document.getElementById("tab-files");
          var transcriptsPanel = document.getElementById("tab-transcripts");
          if (filesPanel && filesPanel.classList.contains("active")) loadFiles();
          if (transcriptsPanel && transcriptsPanel.classList.contains("active")) loadTranscripts();
          if (state.selectedFileId && !document.getElementById("file-detail-panel").classList.contains("hidden")) {
            viewFile(state.selectedFileId);
          }
        }
      }).catch(function () {});
    }, PROCESSING_CHECK_INTERVAL);
  }

  // ================================================================
  // 分页组件
  // ================================================================

  function renderPagination(containerId, currentPage, totalPages, onPageChange) {
    var container = document.getElementById(containerId);
    if (!container) return;
    container.innerHTML = "";
    if (totalPages <= 1) return;

    var prevBtn = document.createElement("button");
    prevBtn.className = "btn btn-secondary btn-sm";
    prevBtn.textContent = "上一页";
    prevBtn.disabled = currentPage <= 1;
    prevBtn.addEventListener("click", function () { if (currentPage > 1) onPageChange(currentPage - 1); });
    container.appendChild(prevBtn);

    var info = document.createElement("span");
    info.className = "page-info";
    info.textContent = "第 " + currentPage + " / " + totalPages + " 页";
    container.appendChild(info);

    var nextBtn = document.createElement("button");
    nextBtn.className = "btn btn-secondary btn-sm";
    nextBtn.textContent = "下一页";
    nextBtn.disabled = currentPage >= totalPages;
    nextBtn.addEventListener("click", function () { if (currentPage < totalPages) onPageChange(currentPage + 1); });
    container.appendChild(nextBtn);
  }

  // ================================================================
  // 删除
  // ================================================================

  function confirmDelete(fileId, filename) {
    state.deleteFileId = fileId;
    document.getElementById("delete-filename").textContent = filename;
    document.getElementById("delete-dialog").classList.remove("hidden");
  }

  function closeDeleteDialog() {
    document.getElementById("delete-dialog").classList.add("hidden");
    state.deleteFileId = null;
  }

  function executeDelete() {
    if (!state.deleteFileId) return;
    var fileId = state.deleteFileId;
    closeDeleteDialog();

    apiDelete("/api/files/" + fileId).then(function (resp) {
      if (resp.code !== 0) { showToast("删除失败: " + resp.message, "error"); return; }
      showToast("已删除", "success");
      if (state.selectedFileId === fileId) {
        document.getElementById("file-detail-panel").classList.add("hidden");
        state.selectedFileId = null;
      }
      state.selectedFileIds.delete(fileId);
      updateBatchBar();
      loadFiles();
    }).catch(function (err) { showToast("删除失败: " + err.message, "error"); });
  }

  // ================================================================
  // 手动触发转写（带模型选择）
  // ================================================================

  function triggerTranscribe(fileId) {
    state.triggerTranscribeFileId = fileId;
    // Load models and show dialog
    apiGet("/api/settings/models").then(function (resp) {
      if (resp.code === 0 && resp.data) {
        var select = document.getElementById("model-select");
        select.innerHTML = "";
        resp.data.forEach(function (model) {
          var opt = document.createElement("option");
          opt.value = model;
          opt.textContent = model.split("/").pop();
          select.appendChild(opt);
        });
        document.getElementById("model-dialog").classList.remove("hidden");
      }
    }).catch(function (err) { showToast("加载模型失败: " + err.message, "error"); });
  }

  function confirmModelTranscribe() {
    var fileId = state.triggerTranscribeFileId;
    var model = document.getElementById("model-select").value;
    if (!fileId) return;
    document.getElementById("model-dialog").classList.add("hidden");

    var url = "/api/transcribe/" + fileId + "?model=" + encodeURIComponent(model);
    apiPost(url).then(function (resp) {
      if (resp.code !== 0) { showToast("触发转写失败: " + resp.message, "error"); return; }
      showToast("已触发转写 (模型: " + model.split("/").pop() + ")", "success");
      loadFiles();
      loadTranscripts();
    }).catch(function (err) { showToast("触发转写失败: " + err.message, "error"); });
  }

  // ================================================================
  // 批量操作
  // ================================================================

  function batchDelete() {
    var ids = Array.from(state.selectedFileIds);
    if (ids.length === 0) return;
    if (!confirm("确定要删除 " + ids.length + " 个文件吗？此操作不可撤销。")) return;

    apiPost("/api/files/batch-delete", { file_ids: ids }).then(function (resp) {
      if (resp.code === 0) {
        showToast("已删除 " + resp.data.deleted_count + " 个文件", "success");
        state.selectedFileIds.clear();
        updateBatchBar();
        loadFiles();
      } else {
        showToast("批量删除失败: " + resp.message, "error");
      }
    }).catch(function (err) { showToast("批量删除失败: " + err.message, "error"); });
  }

  function batchTranscribe() {
    var ids = Array.from(state.selectedFileIds);
    if (ids.length === 0) return;

    apiPost("/api/transcribe/batch", { file_ids: ids }).then(function (resp) {
      if (resp.code === 0) {
        showToast("已触发 " + resp.data.queued_count + " 个文件转写", "success");
        state.selectedFileIds.clear();
        updateBatchBar();
        loadFiles();
      } else {
        showToast("批量转写失败: " + resp.message, "error");
      }
    }).catch(function (err) { showToast("批量转写失败: " + err.message, "error"); });
  }

  // ================================================================
  // 拖拽上传
  // ================================================================

  function initDropZone() {
    var dropZone = document.getElementById("drop-zone");
    var overlay = document.getElementById("drop-zone-overlay");
    if (!dropZone) return;

    var dragCounter = 0;

    dropZone.addEventListener("dragenter", function (e) {
      e.preventDefault();
      dragCounter++;
      overlay.classList.remove("hidden");
      dropZone.classList.add("drag-over");
    });

    dropZone.addEventListener("dragleave", function (e) {
      e.preventDefault();
      dragCounter--;
      if (dragCounter === 0) {
        overlay.classList.add("hidden");
        dropZone.classList.remove("drag-over");
      }
    });

    dropZone.addEventListener("dragover", function (e) {
      e.preventDefault();
    });

    dropZone.addEventListener("drop", function (e) {
      e.preventDefault();
      dragCounter = 0;
      overlay.classList.add("hidden");
      dropZone.classList.remove("drag-over");

      var files = e.dataTransfer.files;
      for (var i = 0; i < files.length; i++) {
        if (files[i].name.toLowerCase().endsWith(".wav")) {
          uploadFile(files[i]);
        } else {
          showToast("仅支持 .wav 文件: " + files[i].name, "error");
        }
      }
    });
  }

  function uploadFile(file) {
    var formData = new FormData();
    formData.append("file", file);

    showToast("正在上传: " + file.name, "info");

    fetch("/upload/web", {
      method: "POST",
      body: formData,
    }).then(function (res) {
      if (!res.ok) throw new Error("HTTP " + res.status);
      return res.json();
    }).then(function (resp) {
      if (resp.code === 0) {
        showToast("上传成功: " + file.name, "success");
        loadFiles();
      } else {
        showToast("上传失败: " + resp.message, "error");
      }
    }).catch(function (err) {
      showToast("上传失败: " + err.message, "error");
    });
  }

  // ================================================================
  // 备份
  // ================================================================

  function backupExport() {
    showToast("正在导出备份...", "info");
    fetch("/api/backup/export").then(function (res) {
      if (!res.ok) throw new Error("HTTP " + res.status);
      return res.blob();
    }).then(function (blob) {
      var url = URL.createObjectURL(blob);
      var a = document.createElement("a");
      a.href = url;
      a.download = "recorder-backup.tar.gz";
      a.click();
      URL.revokeObjectURL(url);
      showToast("备份导出成功", "success");
    }).catch(function (err) {
      showToast("备份导出失败: " + err.message, "error");
    });
  }

  function backupImport(file) {
    if (!file) return;
    var formData = new FormData();
    formData.append("file", file);

    showToast("正在导入备份...", "info");
    fetch("/api/backup/import", {
      method: "POST",
      body: formData,
    }).then(function (res) {
      if (!res.ok) throw new Error("HTTP " + res.status);
      return res.json();
    }).then(function (resp) {
      if (resp.code === 0) {
        showToast("备份导入成功，请重启服务", "success");
      } else {
        showToast("备份导入失败: " + resp.message, "error");
      }
    }).catch(function (err) {
      showToast("备份导入失败: " + err.message, "error");
    });
  }

  // ================================================================
  // 复制到剪贴板
  // ================================================================

  function copyToClipboard(text) {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(function () {
        showToast("已复制到剪贴板", "success");
      }).catch(function () { fallbackCopy(text); });
    } else {
      fallbackCopy(text);
    }
  }

  function fallbackCopy(text) {
    var textarea = document.createElement("textarea");
    textarea.value = text;
    textarea.style.position = "fixed";
    textarea.style.left = "-9999px";
    document.body.appendChild(textarea);
    textarea.select();
    try { document.execCommand("copy"); showToast("已复制到剪贴板", "success"); }
    catch (e) { showToast("复制失败", "error"); }
    document.body.removeChild(textarea);
  }

  // ================================================================
  // 全选
  // ================================================================

  function initSelectAll() {
    var selectAll = document.getElementById("select-all-files");
    if (!selectAll) return;
    selectAll.addEventListener("change", function () {
      var checked = selectAll.checked;
      document.querySelectorAll(".file-checkbox").forEach(function (cb) {
        cb.checked = checked;
        var fid = parseInt(cb.getAttribute("data-file-id"));
        if (checked) state.selectedFileIds.add(fid);
        else state.selectedFileIds.delete(fid);
      });
      updateBatchBar();
    });
  }

  // ================================================================
  // 全局暴露（供 inline onclick 调用）
  // ================================================================
  window._viewFile = viewFile;
  window._viewTranscript = viewTranscript;
  window._confirmDelete = confirmDelete;
  window._triggerTranscribe = triggerTranscribe;
  window._playFile = playFile;

  // ================================================================
  // 初始化
  // ================================================================

  function init() {
    var startTimeStr = window.SERVER_START_TIME || "";
    if (startTimeStr) {
      window.SERVER_START_TS = new Date(startTimeStr).getTime() / 1000;
    } else {
      window.SERVER_START_TS = Date.now() / 1000;
    }

    // 暗色模式
    initTheme();

    // 认证检查
    checkAuth();

    // Tab 切换
    initTabs();

    // 初始加载数据
    loadFiles();
    loadTags();
    loadStatus();

    // 搜索
    initSearch();

    // 拖拽上传
    initDropZone();

    // 全选
    initSelectAll();

    // 登录按钮
    document.getElementById("btn-login").addEventListener("click", function () {
      var password = document.getElementById("login-password").value;
      if (!password) return;
      doLogin(password, false);
    });

    document.getElementById("login-password").addEventListener("keydown", function (e) {
      if (e.key === "Enter") {
        var password = document.getElementById("login-password").value;
        if (password) doLogin(password, false);
      }
    });

    // 退出按钮
    document.getElementById("btn-logout").addEventListener("click", doLogout);

    // 暗色模式切换
    document.getElementById("btn-theme-toggle").addEventListener("click", toggleTheme);

    // 按钮事件
    document.getElementById("btn-refresh-files").addEventListener("click", function () {
      state.filesPage = 1; loadFiles();
    });

    document.getElementById("btn-refresh-transcripts").addEventListener("click", function () {
      state.transcriptsPage = 1; loadTranscripts();
    });

    document.getElementById("btn-refresh-status").addEventListener("click", loadStatus);

    // 保存设置
    document.getElementById("btn-save-settings").addEventListener("click", saveSettings);

    // 关闭详情面板
    document.getElementById("btn-close-detail").addEventListener("click", function () {
      document.getElementById("file-detail-panel").classList.add("hidden");
      state.selectedFileId = null;
      document.querySelectorAll("#files-tbody tr").forEach(function (r) { r.classList.remove("selected"); });
    });

    document.getElementById("btn-close-transcript-detail").addEventListener("click", function () {
      document.getElementById("transcript-detail-panel").classList.add("hidden");
      state.selectedTranscriptFileId = null;
      state.editMode = false;
    });

    // 关闭音频播放器
    document.getElementById("btn-close-player").addEventListener("click", function () {
      var audioPlayer = document.getElementById("audio-player");
      audioPlayer.pause();
      audioPlayer.src = "";
      document.getElementById("audio-player-wrap").classList.add("hidden");
    });

    // 关闭搜索结果
    document.getElementById("btn-close-search").addEventListener("click", function () {
      document.getElementById("search-results").classList.add("hidden");
      document.getElementById("search-input").value = "";
    });

    // 转写记录筛选
    document.getElementById("transcript-status-filter").addEventListener("change", function () {
      state.transcriptsPage = 1; loadTranscripts();
    });

    // 日期筛选
    document.getElementById("date-from").addEventListener("change", function () { state.filesPage = 1; loadFiles(); });
    document.getElementById("date-to").addEventListener("change", function () { state.filesPage = 1; loadFiles(); });

    // 标签筛选
    document.getElementById("tag-filter").addEventListener("change", function () { state.filesPage = 1; loadFiles(); });

    // 删除对话框
    document.getElementById("btn-delete-cancel").addEventListener("click", closeDeleteDialog);
    document.getElementById("btn-delete-confirm").addEventListener("click", executeDelete);
    document.getElementById("delete-dialog").addEventListener("click", function (e) {
      if (e.target === this) closeDeleteDialog();
    });

    // 模型选择对话框
    document.getElementById("btn-model-cancel").addEventListener("click", function () {
      document.getElementById("model-dialog").classList.add("hidden");
    });
    document.getElementById("btn-model-confirm").addEventListener("click", confirmModelTranscribe);

    // 批量操作
    document.getElementById("btn-batch-delete").addEventListener("click", batchDelete);
    document.getElementById("btn-batch-transcribe").addEventListener("click", batchTranscribe);

    // 备份
    document.getElementById("btn-backup-export").addEventListener("click", backupExport);
    document.getElementById("backup-import-file").addEventListener("change", function (e) {
      if (e.target.files && e.target.files[0]) backupImport(e.target.files[0]);
    });

    // 自动刷新
    startStatusAutoRefresh();
    startProcessingCheck();
  }

  // DOM ready
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
