/**
 * ESP32 AI Recorder — Web UI 前端交互逻辑 v0.4
 *
 * 功能：
 * - Tab 切换（4 tabs）
 * - 认证（自动登录 / 手动登录 / 退出）
 * - 录音列表分页（AJAX /api/files）+ 日期筛选
 * - 转写记录列表分页（AJAX /api/transcripts）
 * - 转写结果展示（点击文件行加载详情）
 * - 转写编辑（textarea + 保存）
 * - 音频播放（<audio> 标签 + /stream 端点）
 * - 全局搜索（debounce 300ms，AJAX /api/search）
 * - 设置 Tab（加载 / 保存设置，模型列表）
 * - 系统状态自动刷新（每 10 秒轮询 /api/status）
 * - 删除确认对话框
 * - 复制转写文本到剪贴板
 * - 手动触发转写
 */

(function () {
  "use strict";

  // ================================================================
  // 常量
  // ================================================================
  var PAGE_SIZE = 20;
  var STATUS_REFRESH_INTERVAL = 10000; // 10 秒
  var PROCESSING_CHECK_INTERVAL = 5000; // 5 秒
  var SEARCH_DEBOUNCE_MS = 300;

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
  };

  // ================================================================
  // 工具函数
  // ================================================================

  /** 格式化文件大小 */
  function formatSize(bytes) {
    if (bytes < 1024) return bytes + " B";
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB";
    return (bytes / (1024 * 1024)).toFixed(1) + " MB";
  }

  /** 格式化日期时间 */
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

  /** 格式化时长（秒 → mm:ss） */
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

  /** 格式化运行时间 */
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

  /** 状态图标和文本 */
  function statusBadge(status) {
    var icons = {
      completed: "✅",
      processing: "⏳",
      pending: "🕒",
      failed: "❌",
    };
    var labels = {
      completed: "已完成",
      processing: "转写中",
      pending: "等待中",
      failed: "失败",
    };
    var icon = icons[status] || "❓";
    var label = labels[status] || status;
    return (
      '<span class="status-badge ' + status + '">' + icon + " " + label + "</span>"
    );
  }

  /** 显示 Toast 通知 */
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

  /** AJAX GET 请求 */
  function apiGet(url) {
    return fetch(url).then(function (res) {
      if (res.status === 401) {
        showLoginOverlay();
        throw new Error("Unauthorized");
      }
      if (!res.ok) throw new Error("HTTP " + res.status);
      return res.json();
    });
  }

  /** AJAX DELETE 请求 */
  function apiDelete(url) {
    return fetch(url, { method: "DELETE" }).then(function (res) {
      if (!res.ok) throw new Error("HTTP " + res.status);
      return res.json();
    });
  }

  /** AJAX POST 请求（可带 body） */
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

  /** AJAX PUT 请求 */
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
          if (!isAuto) {
            showToast("登录成功", "success");
          }
        } else {
          if (isAuto) {
            // 自动登录失败，显示登录界面
            localStorage.removeItem("rec_password");
            showLoginOverlay();
          } else {
            var errorEl = document.getElementById("login-error");
            errorEl.textContent = "密码错误";
            errorEl.classList.remove("hidden");
          }
        }
      })
      .catch(function () {
        if (isAuto) {
          showLoginOverlay();
        } else {
          showToast("登录失败", "error");
        }
      });
  }

  function doLogout() {
    apiPost("/api/auth/logout")
      .then(function () {
        localStorage.removeItem("rec_password");
        showLoginOverlay();
        showToast("已退出登录", "info");
      })
      .catch(function () {
        localStorage.removeItem("rec_password");
        showLoginOverlay();
      });
  }

  // ================================================================
  // Tab 切换
  // ================================================================

  function initTabs() {
    var btns = document.querySelectorAll(".tab-btn");
    btns.forEach(function (btn) {
      btn.addEventListener("click", function () {
        var tab = btn.getAttribute("data-tab");
        // 更新按钮状态
        btns.forEach(function (b) {
          b.classList.remove("active");
        });
        btn.classList.add("active");
        // 更新面板
        document.querySelectorAll(".tab-panel").forEach(function (panel) {
          panel.classList.remove("active");
        });
        var target = document.getElementById("tab-" + tab);
        if (target) target.classList.add("active");
        // 切换时刷新对应数据
        if (tab === "files") {
          loadFiles();
        } else if (tab === "transcripts") {
          loadTranscripts();
        } else if (tab === "settings") {
          loadSettings();
        } else if (tab === "status") {
          loadStatus();
        }
      });
    });
  }

  // ================================================================
  // 录音列表
  // ================================================================

  function loadFiles() {
    var url =
      "/api/files?page=" +
      state.filesPage +
      "&page_size=" +
      PAGE_SIZE +
      "&sort=upload_time&order=desc";

    // 日期筛选
    var dateFrom = document.getElementById("date-from").value;
    var dateTo = document.getElementById("date-to").value;
    if (dateFrom) url += "&date_from=" + encodeURIComponent(dateFrom);
    if (dateTo) url += "&date_to=" + encodeURIComponent(dateTo);

    apiGet(url)
      .then(function (resp) {
        if (resp.code !== 0) {
          showToast("加载文件列表失败: " + resp.message, "error");
          return;
        }
        var data = resp.data;
        state.filesTotal = data.total;
        renderFilesTable(data.items);
        renderPagination(
          "files-pagination",
          data.page,
          Math.ceil(data.total / data.page_size),
          function (page) {
            state.filesPage = page;
            loadFiles();
          }
        );
      })
      .catch(function (err) {
        if (err.message !== "Unauthorized") {
          showToast("网络错误: " + err.message, "error");
        }
      });
  }

  function renderFilesTable(items) {
    var tbody = document.getElementById("files-tbody");
    if (!items || items.length === 0) {
      tbody.innerHTML =
        '<tr><td colspan="6" class="empty-msg">暂无录音文件</td></tr>';
      return;
    }

    var html = "";
    items.forEach(function (item) {
      var transStatus = item.transcription
        ? item.transcription.status
        : "pending";
      var isSelected = item.id === state.selectedFileId;

      html +=
        '<tr class="' +
        (isSelected ? "selected" : "") +
        '" data-file-id="' +
        item.id +
        '">';
      html +=
        '<td class="text-mono" title="' +
        escapeHtml(item.filename) +
        '">' +
        escapeHtml(truncate(item.filename, 30)) +
        "</td>";
      html += "<td>" + formatDuration(item.duration) + "</td>";
      html += "<td>" + formatSize(item.file_size) + "</td>";
      html += "<td>" + formatDateTime(item.upload_time) + "</td>";
      html += "<td>" + statusBadge(transStatus) + "</td>";
      html += "<td>";
      html +=
        '<button class="btn-icon" title="播放" onclick="window._playFile(' +
        item.id +
        ')">▶</button> ';
      html +=
        '<button class="btn-icon" title="查看详情" onclick="window._viewFile(' +
        item.id +
        ')">👁</button> ';
      if (transStatus === "completed" || transStatus === "failed") {
        html +=
          '<button class="btn-icon" title="重新转写" onclick="window._triggerTranscribe(' +
          item.id +
          ')">🔄</button> ';
      }
      html +=
        '<button class="btn-icon danger" title="删除" onclick="window._confirmDelete(' +
        item.id +
        ", '" +
        escapeHtml(item.filename).replace(/'/g, "\\'") +
        "'" +
        ')">🗑</button>';
      html += "</td>";
      html += "</tr>";
    });
    tbody.innerHTML = html;
  }

  /** 查看文件转写详情 */
  function viewFile(fileId) {
    state.selectedFileId = fileId;

    // 高亮选中行
    var rows = document.querySelectorAll("#files-tbody tr");
    rows.forEach(function (r) {
      r.classList.remove("selected");
      if (r.getAttribute("data-file-id") === String(fileId)) {
        r.classList.add("selected");
      }
    });

    var panel = document.getElementById("file-detail-panel");
    panel.classList.remove("hidden");

    var filenameEl = document.getElementById("detail-filename");
    var metaEl = document.getElementById("detail-meta");
    var contentEl = document.getElementById("detail-content");
    var actionsEl = document.getElementById("detail-actions");

    contentEl.innerHTML = '<p class="empty-msg">加载中...</p>';
    actionsEl.classList.add("hidden");

    // 并行加载文件详情和转写详情
    Promise.all([
      apiGet("/api/files/" + fileId),
      apiGet("/api/transcripts/" + fileId),
    ])
      .then(function (results) {
        var fileResp = results[0];
        var transResp = results[1];

        if (fileResp.code !== 0) {
          contentEl.innerHTML =
            '<p class="empty-msg">加载文件信息失败</p>';
          return;
        }

        var file = fileResp.data;
        filenameEl.textContent = file.filename;
        var metaParts = [];
        if (file.duration) metaParts.push(formatDuration(file.duration));
        metaParts.push(formatSize(file.file_size));
        metaParts.push(formatDateTime(file.upload_time));
        metaEl.textContent = metaParts.join(" | ");

        if (transResp.code !== 0 || !transResp.data) {
          contentEl.innerHTML =
            '<p class="empty-msg">暂无转写记录</p>';
          return;
        }

        var trans = transResp.data;

        if (trans.status === "completed" && trans.text) {
          contentEl.textContent = trans.text;
          actionsEl.classList.remove("hidden");

          // 设置下载按钮
          document.getElementById("btn-download-txt").onclick = function () {
            window.open("/api/transcripts/" + fileId + "/export", "_blank");
          };

          // 设置复制按钮
          document.getElementById("btn-copy-txt").onclick = function () {
            copyToClipboard(trans.text);
          };
        } else if (trans.status === "failed") {
          contentEl.innerHTML =
            '<p class="empty-msg" style="color:var(--color-danger)">转写失败: ' +
            escapeHtml(trans.error_msg || "未知错误") +
            "</p>";
          actionsEl.classList.remove("hidden");
          document.getElementById("btn-download-txt").style.display = "none";
          document.getElementById("btn-copy-txt").style.display = "none";
        } else if (trans.status === "processing") {
          contentEl.innerHTML =
            '<p class="empty-msg">⏳ 正在转写中，请稍后刷新查看...</p>';
        } else {
          contentEl.innerHTML =
            '<p class="empty-msg">等待转写中...</p>';
        }
      })
      .catch(function (err) {
        contentEl.innerHTML =
          '<p class="empty-msg">加载失败: ' + escapeHtml(err.message) + "</p>";
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

    // 获取文件名
    apiGet("/api/files/" + fileId).then(function (resp) {
      if (resp.code === 0 && resp.data) {
        playerName.textContent = resp.data.filename;
      }
    }).catch(function () {});

    audioPlayer.play().catch(function () {});
  }

  // ================================================================
  // 转写记录
  // ================================================================

  function loadTranscripts() {
    var statusFilter = document.getElementById(
      "transcript-status-filter"
    ).value;
    var url =
      "/api/transcripts?page=" +
      state.transcriptsPage +
      "&page_size=" +
      PAGE_SIZE;
    if (statusFilter) {
      url += "&status=" + encodeURIComponent(statusFilter);
    }

    apiGet(url)
      .then(function (resp) {
        if (resp.code !== 0) {
          showToast("加载转写记录失败: " + resp.message, "error");
          return;
        }
        var data = resp.data;
        state.transcriptsTotal = data.total;
        renderTranscriptsTable(data.items);
        renderPagination(
          "transcripts-pagination",
          data.page,
          Math.ceil(data.total / data.page_size),
          function (page) {
            state.transcriptsPage = page;
            loadTranscripts();
          }
        );
      })
      .catch(function (err) {
        showToast("网络错误: " + err.message, "error");
      });
  }

  function renderTranscriptsTable(items) {
    var tbody = document.getElementById("transcripts-tbody");
    if (!items || items.length === 0) {
      tbody.innerHTML =
        '<tr><td colspan="6" class="empty-msg">暂无转写记录</td></tr>';
      return;
    }

    var html = "";
    items.forEach(function (item) {
      html +=
        '<tr data-file-id="' +
        item.file_id +
        '" data-trans-id="' +
        item.id +
        '">';
      html +=
        '<td><a href="javascript:void(0)" onclick="window._viewTranscript(' +
        item.file_id +
        ')">#' +
        item.file_id +
        "</a></td>";
      html += "<td>" + statusBadge(item.status) + "</td>";
      html += "<td>" + (item.language || "—") + "</td>";
      html += "<td>" + formatDuration(item.duration) + "</td>";
      html += "<td>" + formatDateTime(item.completed_at) + "</td>";
      html += "<td>";
      html +=
        '<button class="btn-icon" title="查看详情" onclick="window._viewTranscript(' +
        item.file_id +
        ')">👁</button> ';
      if (item.status === "completed" || item.status === "failed") {
        html +=
          '<button class="btn-icon" title="重新转写" onclick="window._triggerTranscribe(' +
          item.file_id +
          ')">🔄</button>';
      }
      html += "</td>";
      html += "</tr>";
    });
    tbody.innerHTML = html;
  }

  /** 查看转写详情 */
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
    // Reset download/copy visibility
    document.getElementById("btn-download-transcript-txt").style.display = "";
    document.getElementById("btn-copy-transcript-txt").style.display = "";
    btnEdit.style.display = "";

    Promise.all([
      apiGet("/api/files/" + fileId),
      apiGet("/api/transcripts/" + fileId),
    ])
      .then(function (results) {
        var fileResp = results[0];
        var transResp = results[1];

        var filename = "File #" + fileId;
        if (fileResp.code === 0 && fileResp.data) {
          filename = fileResp.data.filename;
        }
        titleEl.textContent = filename;

        if (transResp.code !== 0 || !transResp.data) {
          contentEl.innerHTML =
            '<p class="empty-msg">暂无转写记录</p>';
          return;
        }

        var trans = transResp.data;
        state.currentTranscriptText = trans.text || "";

        // 显示已编辑标记
        if (trans.is_edited === 1) {
          editBadge.classList.remove("hidden");
          if (trans.edited_at) {
            editedAtEl.textContent = "编辑于 " + formatDateTime(trans.edited_at);
          }
        }

        if (trans.status === "completed" && trans.text) {
          // 使用 textarea 显示转写文本（方便编辑）
          var textarea = document.createElement("textarea");
          textarea.id = "transcript-text-editor";
          textarea.className = "transcript-editor";
          textarea.value = trans.text;
          textarea.readOnly = true;
          contentEl.innerHTML = "";
          contentEl.appendChild(textarea);

          actionsEl.classList.remove("hidden");

          document.getElementById(
            "btn-download-transcript-txt"
          ).onclick = function () {
            window.open("/api/transcripts/" + fileId + "/export", "_blank");
          };

          document.getElementById(
            "btn-copy-transcript-txt"
          ).onclick = function () {
            copyToClipboard(textarea.value);
          };

          btnEdit.onclick = function () {
            state.editMode = true;
            textarea.readOnly = false;
            textarea.focus();
            btnEdit.style.display = "none";
            btnSave.classList.remove("hidden");
          };

          btnSave.onclick = function () {
            saveTranscript(fileId, textarea.value);
          };

        } else if (trans.status === "failed") {
          contentEl.innerHTML =
            '<p class="empty-msg" style="color:var(--color-danger)">转写失败: ' +
            escapeHtml(trans.error_msg || "未知错误") +
            "</p>";
        } else if (trans.status === "processing") {
          contentEl.innerHTML =
            '<p class="empty-msg">⏳ 正在转写中...</p>';
        } else {
          contentEl.innerHTML =
            '<p class="empty-msg">等待转写中...</p>';
        }
      })
      .catch(function (err) {
        contentEl.innerHTML =
          '<p class="empty-msg">加载失败: ' + escapeHtml(err.message) + "</p>";
      });
  }

  /** 保存转写编辑 */
  function saveTranscript(fileId, text) {
    apiPut("/api/transcripts/" + fileId, { text: text })
      .then(function (resp) {
        if (resp.code === 0) {
          showToast("保存成功", "success");
          state.editMode = false;
          // 重新加载详情
          viewTranscript(fileId);
        } else {
          showToast("保存失败: " + resp.message, "error");
        }
      })
      .catch(function (err) {
        showToast("保存失败: " + err.message, "error");
      });
  }

  // ================================================================
  // 设置 Tab
  // ================================================================

  function loadSettings() {
    Promise.all([
      apiGet("/api/settings"),
      apiGet("/api/settings/models"),
    ])
      .then(function (results) {
        var settingsResp = results[0];
        var modelsResp = results[1];

        if (settingsResp.code !== 0) return;
        var settings = settingsResp.data;

        // 语言下拉
        var langSelect = document.getElementById("setting-language");
        if (settings.transcribe_language) {
          langSelect.value = settings.transcribe_language;
        }

        // 模型下拉
        var modelSelect = document.getElementById("setting-model");
        if (modelsResp.code === 0 && modelsResp.data) {
          modelSelect.innerHTML = "";
          modelsResp.data.forEach(function (model) {
            var opt = document.createElement("option");
            opt.value = model;
            opt.textContent = model.split("/").pop();
            modelSelect.appendChild(opt);
          });
          if (settings.transcribe_model) {
            modelSelect.value = settings.transcribe_model;
          }
        }

        // 自动转写开关
        var autoToggle = document.getElementById("setting-auto-transcribe");
        autoToggle.checked = settings.auto_transcribe !== "false";
      })
      .catch(function () {});
  }

  function saveSettings() {
    var data = {
      transcribe_language: document.getElementById("setting-language").value,
      transcribe_model: document.getElementById("setting-model").value,
      auto_transcribe: document.getElementById("setting-auto-transcribe").checked ? "true" : "false",
    };

    apiPut("/api/settings", data)
      .then(function (resp) {
        if (resp.code === 0) {
          showToast("设置已保存", "success");
        } else {
          showToast("保存失败: " + resp.message, "error");
        }
      })
      .catch(function (err) {
        showToast("保存失败: " + err.message, "error");
      });
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
            // 将 **keyword** 替换为 <mark>keyword</mark>
            var snippet = escapeHtml(item.snippet || "")
              .replace(/\*\*/g, function () { return "§§MARK§§"; });
            // 简单替换：将 §§MARK§§keyword§§MARK§§ 替换为 <mark>
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
        if (err.message !== "Unauthorized") {
          showToast("搜索失败: " + err.message, "error");
        }
      });
  }

  function initSearch() {
    var input = document.getElementById("search-input");
    input.addEventListener("input", function () {
      var val = input.value;
      if (state.searchTimer) clearTimeout(state.searchTimer);
      state.searchTimer = setTimeout(function () {
        doSearch(val);
      }, SEARCH_DEBOUNCE_MS);
    });
  }

  // ================================================================
  // 系统状态
  // ================================================================

  function loadStatus() {
    apiGet("/api/status")
      .then(function (resp) {
        if (resp.code !== 0) return;
        var data = resp.data;

        // 磁盘
        var diskUsed = data.disk_used_bytes;
        var diskTotal = data.disk_total_bytes;
        var diskPercent =
          diskTotal > 0 ? ((diskUsed / diskTotal) * 100).toFixed(1) : 0;
        document.getElementById("stat-disk").textContent =
          formatSize(diskUsed) + " / " + formatSize(diskTotal);
        document.getElementById("stat-disk-bar").style.width =
          diskPercent + "%";

        // 文件数
        document.getElementById("stat-files").textContent = data.file_count;

        // 转写统计
        var stats = data.transcription_stats;
        document.getElementById("stat-transcripts").textContent =
          stats.completed + " 完成";
        document.getElementById("stat-transcripts-detail").textContent =
          "等待: " +
          stats.pending +
          " | 处理中: " +
          stats.processing +
          " | 失败: " +
          stats.failed +
          " | 总计: " +
          stats.total;

        // Whisper 状态（根据是否有 processing 或 pending 来判断）
        var whisperEl = document.getElementById("stat-whisper");
        if (stats.processing > 0) {
          whisperEl.textContent = "🔄 转写中";
          whisperEl.style.color = "var(--color-warning)";
        } else if (stats.pending > 0) {
          whisperEl.textContent = "⏳ 待处理";
          whisperEl.style.color = "var(--color-info)";
        } else {
          whisperEl.textContent = "✅ 就绪";
          whisperEl.style.color = "var(--color-success)";
        }

        // 运行时间
        document.getElementById("stat-uptime").textContent = formatUptime(
          window.SERVER_START_TS
        );
      })
      .catch(function () {
        // 静默失败
      });
  }

  function startStatusAutoRefresh() {
    if (state.statusTimer) clearInterval(state.statusTimer);
    state.statusTimer = setInterval(function () {
      // 只在系统状态 Tab 可见时刷新
      var statusPanel = document.getElementById("tab-status");
      if (statusPanel && statusPanel.classList.contains("active")) {
        loadStatus();
      }
    }, STATUS_REFRESH_INTERVAL);
  }

  /** 检查是否有正在转写中的文件，如果有则刷新列表 */
  function startProcessingCheck() {
    if (state.processingCheckTimer)
      clearInterval(state.processingCheckTimer);
    state.processingCheckTimer = setInterval(function () {
      apiGet("/api/status")
        .then(function (resp) {
          if (resp.code !== 0) return;
          var stats = resp.data.transcription_stats;
          var hasActive =
            stats.processing > 0 || stats.pending > 0;
          if (hasActive) {
            var filesPanel = document.getElementById("tab-files");
            var transcriptsPanel = document.getElementById(
              "tab-transcripts"
            );
            if (filesPanel && filesPanel.classList.contains("active")) {
              loadFiles();
            }
            if (
              transcriptsPanel &&
              transcriptsPanel.classList.contains("active")
            ) {
              loadTranscripts();
            }
            // 如果有详情面板打开，也刷新
            if (
              state.selectedFileId &&
              !document.getElementById("file-detail-panel").classList.contains("hidden")
            ) {
              viewFile(state.selectedFileId);
            }
          }
        })
        .catch(function () {});
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
    prevBtn.addEventListener("click", function () {
      if (currentPage > 1) onPageChange(currentPage - 1);
    });
    container.appendChild(prevBtn);

    var info = document.createElement("span");
    info.className = "page-info";
    info.textContent = "第 " + currentPage + " / " + totalPages + " 页";
    container.appendChild(info);

    var nextBtn = document.createElement("button");
    nextBtn.className = "btn btn-secondary btn-sm";
    nextBtn.textContent = "下一页";
    nextBtn.disabled = currentPage >= totalPages;
    nextBtn.addEventListener("click", function () {
      if (currentPage < totalPages) onPageChange(currentPage + 1);
    });
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

    apiDelete("/api/files/" + fileId)
      .then(function (resp) {
        if (resp.code !== 0) {
          showToast("删除失败: " + resp.message, "error");
          return;
        }
        showToast("已删除", "success");
        // 如果当前正在查看该文件，关闭详情
        if (state.selectedFileId === fileId) {
          document.getElementById("file-detail-panel").classList.add("hidden");
          state.selectedFileId = null;
        }
        loadFiles();
      })
      .catch(function (err) {
        showToast("删除失败: " + err.message, "error");
      });
  }

  // ================================================================
  // 手动触发转写
  // ================================================================

  function triggerTranscribe(fileId) {
    apiPost("/api/transcribe/" + fileId)
      .then(function (resp) {
        if (resp.code !== 0) {
          showToast("触发转写失败: " + resp.message, "error");
          return;
        }
        showToast("已触发转写", "success");
        loadFiles();
        loadTranscripts();
      })
      .catch(function (err) {
        showToast("触发转写失败: " + err.message, "error");
      });
  }

  // ================================================================
  // 复制到剪贴板
  // ================================================================

  function copyToClipboard(text) {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard
        .writeText(text)
        .then(function () {
          showToast("已复制到剪贴板", "success");
        })
        .catch(function () {
          fallbackCopy(text);
        });
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
    try {
      document.execCommand("copy");
      showToast("已复制到剪贴板", "success");
    } catch (e) {
      showToast("复制失败", "error");
    }
    document.body.removeChild(textarea);
  }

  // ================================================================
  // 工具
  // ================================================================

  function escapeHtml(str) {
    if (!str) return "";
    return str
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;")
      .replace(/'/g, "&#039;");
  }

  function truncate(str, maxLen) {
    if (!str) return "";
    if (str.length <= maxLen) return str;
    return str.substring(0, maxLen - 3) + "...";
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
    // 解析服务器启动时间
    var startTimeStr = window.SERVER_START_TIME || "";
    if (startTimeStr) {
      window.SERVER_START_TS = new Date(startTimeStr).getTime() / 1000;
    } else {
      window.SERVER_START_TS = Date.now() / 1000;
    }

    // 认证检查
    checkAuth();

    // Tab 切换
    initTabs();

    // 初始加载数据
    loadFiles();
    loadStatus();

    // 搜索
    initSearch();

    // 登录按钮
    document.getElementById("btn-login").addEventListener("click", function () {
      var password = document.getElementById("login-password").value;
      if (!password) return;
      doLogin(password, false);
    });

    // 登录输入框回车
    document.getElementById("login-password").addEventListener("keydown", function (e) {
      if (e.key === "Enter") {
        var password = document.getElementById("login-password").value;
        if (password) doLogin(password, false);
      }
    });

    // 退出按钮
    document.getElementById("btn-logout").addEventListener("click", doLogout);

    // 按钮事件
    document
      .getElementById("btn-refresh-files")
      .addEventListener("click", function () {
        state.filesPage = 1;
        loadFiles();
      });

    document
      .getElementById("btn-refresh-transcripts")
      .addEventListener("click", function () {
        state.transcriptsPage = 1;
        loadTranscripts();
      });

    document
      .getElementById("btn-refresh-status")
      .addEventListener("click", loadStatus);

    // 保存设置
    document
      .getElementById("btn-save-settings")
      .addEventListener("click", saveSettings);

    // 关闭详情面板
    document
      .getElementById("btn-close-detail")
      .addEventListener("click", function () {
        document.getElementById("file-detail-panel").classList.add("hidden");
        state.selectedFileId = null;
        // 取消行选中
        document
          .querySelectorAll("#files-tbody tr")
          .forEach(function (r) {
            r.classList.remove("selected");
          });
      });

    document
      .getElementById("btn-close-transcript-detail")
      .addEventListener("click", function () {
        document
          .getElementById("transcript-detail-panel")
          .classList.add("hidden");
        state.selectedTranscriptFileId = null;
        state.editMode = false;
      });

    // 关闭音频播放器
    document
      .getElementById("btn-close-player")
      .addEventListener("click", function () {
        var audioPlayer = document.getElementById("audio-player");
        audioPlayer.pause();
        audioPlayer.src = "";
        document.getElementById("audio-player-wrap").classList.add("hidden");
      });

    // 关闭搜索结果
    document
      .getElementById("btn-close-search")
      .addEventListener("click", function () {
        document.getElementById("search-results").classList.add("hidden");
        document.getElementById("search-input").value = "";
      });

    // 转写记录筛选
    document
      .getElementById("transcript-status-filter")
      .addEventListener("change", function () {
        state.transcriptsPage = 1;
        loadTranscripts();
      });

    // 日期筛选
    document.getElementById("date-from").addEventListener("change", function () {
      state.filesPage = 1;
      loadFiles();
    });
    document.getElementById("date-to").addEventListener("change", function () {
      state.filesPage = 1;
      loadFiles();
    });

    // 删除对话框
    document
      .getElementById("btn-delete-cancel")
      .addEventListener("click", closeDeleteDialog);
    document
      .getElementById("btn-delete-confirm")
      .addEventListener("click", executeDelete);

    // 点击对话框外部关闭
    document
      .getElementById("delete-dialog")
      .addEventListener("click", function (e) {
        if (e.target === this) closeDeleteDialog();
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
