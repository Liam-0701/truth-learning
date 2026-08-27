const state = {
  documents: [],
  selectedIds: new Set(),
  quiz: null,
  study: { today_seconds: 0, total_seconds: 0, active_days: 0, recent: [] },
  pendingSeconds: 0,
  lastActivity: Date.now(),
  timerStarted: false,
};

const $ = (selector) => document.querySelector(selector);
const elements = {
  systemBadge: $("#systemBadge"),
  documentCount: $("#documentCount"),
  documentList: $("#documentList"),
  fileInput: $("#fileInput"),
  uploadZone: $("#uploadZone"),
  uploadStatus: $("#uploadStatus"),
  selectAll: $("#selectAll"),
  chatForm: $("#chatForm"),
  questionInput: $("#questionInput"),
  chatWindow: $("#chatWindow"),
  quizForm: $("#quizForm"),
  quizTopic: $("#quizTopic"),
  quizCount: $("#quizCount"),
  quizContent: $("#quizContent"),
  quizRecord: $("#quizRecord"),
  todayHours: $("#todayHours"),
  encouragement: $("#encouragement"),
  focusProgress: $("#focusProgress"),
  totalLearning: $("#totalLearning"),
  profileToday: $("#profileToday"),
  activeDays: $("#activeDays"),
  historyChart: $("#historyChart"),
  profileModal: $("#profileModal"),
  toast: $("#toast"),
  footerStats: $("#footerStats"),
};

function escapeHtml(value) {
  return String(value ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

function simpleText(value) {
  return escapeHtml(value)
    .replace(/\*\*(.+?)\*\*/g, "<strong>$1</strong>")
    .replace(/\n/g, "<br>");
}

async function api(path, options = {}) {
  const response = await fetch(path, options);
  const data = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(data.error || `请求失败（${response.status}）`);
  return data;
}

let toastTimer;
function toast(message) {
  elements.toast.textContent = message;
  elements.toast.classList.remove("hidden");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => elements.toast.classList.add("hidden"), 3200);
}

function localDate(date = new Date()) {
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, "0");
  const day = String(date.getDate()).padStart(2, "0");
  return `${year}-${month}-${day}`;
}

function selectedDocumentIds() {
  return [...state.selectedIds];
}

async function loadHealth() {
  try {
    const health = await api("/api/health");
    elements.systemBadge.classList.add("ready");
    elements.systemBadge.innerHTML = `<i></i> ${health.openai_enabled ? "AI 已连接" : "本地检索模式"}`;
    elements.systemBadge.title = health.document_support;
  } catch {
    elements.systemBadge.textContent = "服务未连接";
  }
}

async function loadDocuments() {
  const data = await api("/api/documents");
  const previous = state.selectedIds;
  state.documents = data.documents || [];
  state.selectedIds = new Set(
    state.documents.filter((document) => previous.size === 0 || previous.has(document.id)).map((document) => document.id),
  );
  renderDocuments();
}

function renderDocuments() {
  elements.documentCount.textContent = `${state.documents.length} 份资料`;
  if (!state.documents.length) {
    elements.documentList.innerHTML = '<div class="empty-state compact">还没有资料。上传第一份课件，开始构建你的知识空间。</div>';
    return;
  }
  elements.documentList.innerHTML = state.documents
    .map((document) => {
      const type = escapeHtml((document.source_type || "FILE").toUpperCase());
      const detail = `${document.page_count || 1} 页 · ${document.chunk_count || 0} 个片段`;
      return `
        <div class="document-card">
          <input type="checkbox" data-document-select="${document.id}" ${state.selectedIds.has(document.id) ? "checked" : ""} aria-label="选择 ${escapeHtml(document.name)}">
          <span class="file-icon">${type}</span>
          <div class="document-meta">
            <strong title="${escapeHtml(document.name)}">${escapeHtml(document.name)}</strong>
            <span>${escapeHtml(detail)}</span>
          </div>
          <button class="delete-document" type="button" data-document-delete="${document.id}" aria-label="删除 ${escapeHtml(document.name)}">×</button>
        </div>`;
    })
    .join("");

  document.querySelectorAll("[data-document-select]").forEach((checkbox) => {
    checkbox.addEventListener("change", () => {
      const id = Number(checkbox.dataset.documentSelect);
      checkbox.checked ? state.selectedIds.add(id) : state.selectedIds.delete(id);
    });
  });
  document.querySelectorAll("[data-document-delete]").forEach((button) => {
    button.addEventListener("click", async () => {
      const id = Number(button.dataset.documentDelete);
      const document = state.documents.find((item) => item.id === id);
      if (!confirm(`删除“${document?.name || "这份资料"}”及其全部索引吗？`)) return;
      try {
        await api(`/api/documents?id=${id}`, { method: "DELETE" });
        state.selectedIds.delete(id);
        await Promise.all([loadDocuments(), loadStats()]);
        toast("资料已删除");
      } catch (error) {
        toast(error.message);
      }
    });
  });
}

async function uploadFiles(files) {
  const accepted = [...files];
  if (!accepted.length) return;
  elements.uploadStatus.classList.remove("hidden");
  elements.fileInput.disabled = true;
  let completed = 0;
  try {
    for (const file of accepted) {
      elements.uploadStatus.textContent = `正在提取并建立向量索引：${file.name}（${completed + 1}/${accepted.length}）`;
      const form = new FormData();
      form.append("file", file);
      await api("/api/documents", { method: "POST", body: form });
      completed += 1;
    }
    elements.uploadStatus.textContent = `已完成 ${completed} 份资料的文字提取、切片与向量索引。`;
    await Promise.all([loadDocuments(), loadStats()]);
    toast("资料已经进入你的知识库");
  } catch (error) {
    elements.uploadStatus.textContent = `处理失败：${error.message}`;
    toast(error.message);
  } finally {
    elements.fileInput.disabled = false;
    elements.fileInput.value = "";
  }
}

function addMessage(role, html, sources = []) {
  const wrapper = document.createElement("div");
  wrapper.className = `message ${role === "user" ? "user-message" : "assistant-message"}`;
  const sourceHtml = sources.length
    ? `<div class="source-list">${sources
        .slice(0, 4)
        .map(
          (source) =>
            `<div class="source-chip">《${escapeHtml(source.document_name)}》第 ${source.page} 页 · ${escapeHtml(source.excerpt)}</div>`,
        )
        .join("")}</div>`
    : "";
  wrapper.innerHTML = role === "user"
    ? `<div>${html}</div>`
    : `<span class="message-avatar">T</span><div>${html}${sourceHtml}</div>`;
  elements.chatWindow.appendChild(wrapper);
  elements.chatWindow.scrollTop = elements.chatWindow.scrollHeight;
  return wrapper;
}

async function askQuestion(question) {
  if (!question.trim()) return;
  addMessage("user", simpleText(question));
  elements.questionInput.value = "";
  const submit = elements.chatForm.querySelector("button");
  submit.disabled = true;
  const typing = addMessage("assistant", '<span class="typing">正在资料中寻找依据 ···</span>');
  try {
    const result = await api("/api/chat", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ question, document_ids: selectedDocumentIds() }),
    });
    typing.remove();
    addMessage("assistant", simpleText(result.answer), result.sources || []);
  } catch (error) {
    typing.remove();
    addMessage("assistant", `抱歉，${escapeHtml(error.message)}`);
  } finally {
    submit.disabled = false;
    elements.questionInput.focus();
  }
}

function renderQuiz() {
  if (!state.quiz?.questions?.length) {
    elements.quizContent.innerHTML = '<div class="empty-state quiz-empty"><span>?</span><strong>暂时无法生成题目</strong><p>请先上传并选择至少一份资料。</p></div>';
    return;
  }
  const questions = state.quiz.questions
    .map(
      (question, index) => `
        <section class="quiz-question" data-question="${index}">
          <p>${index + 1}. ${escapeHtml(question.question)}</p>
          <div class="quiz-options">
            ${question.options
              .map(
                (option, optionIndex) => `
                  <label class="quiz-option" data-option="${optionIndex}">
                    <input type="radio" name="question-${index}" value="${optionIndex}">
                    <span>${escapeHtml(option)}</span>
                  </label>`,
              )
              .join("")}
          </div>
          <div class="quiz-explanation hidden"></div>
        </section>`,
    )
    .join("");
  elements.quizContent.innerHTML = `
    <div class="quiz-result hidden" id="quizResult"></div>
    ${questions}
    <button class="primary-button quiz-submit" id="submitQuiz" type="button">提交答案</button>`;
  $("#submitQuiz").addEventListener("click", submitQuiz);
}

async function submitQuiz() {
  let score = 0;
  let answered = 0;
  state.quiz.questions.forEach((question, index) => {
    const container = document.querySelector(`[data-question="${index}"]`);
    const selected = container.querySelector(`input[name="question-${index}"]:checked`);
    if (selected) answered += 1;
    const selectedIndex = selected ? Number(selected.value) : -1;
    if (selectedIndex === question.answer_index) score += 1;
    container.querySelectorAll(".quiz-option").forEach((option) => {
      const indexValue = Number(option.dataset.option);
      if (indexValue === question.answer_index) option.classList.add("correct");
      else if (indexValue === selectedIndex) option.classList.add("wrong");
      option.querySelector("input").disabled = true;
    });
    const explanation = container.querySelector(".quiz-explanation");
    explanation.classList.remove("hidden");
    explanation.innerHTML = `${escapeHtml(question.explanation)}<br>来源：《${escapeHtml(question.source.document_name)}》第 ${question.source.page} 页`;
  });
  if (answered < state.quiz.questions.length) toast(`还有 ${state.quiz.questions.length - answered} 题未作答，已按未答计分。`);
  const percent = Math.round((score / state.quiz.questions.length) * 100);
  const result = $("#quizResult");
  result.classList.remove("hidden");
  result.innerHTML = `<strong>${score}/${state.quiz.questions.length}</strong><br>${percent >= 80 ? "理解很稳，试着隔一天再回忆一次。" : "找到模糊处了，这正是练习的价值。"}`;
  $("#submitQuiz").disabled = true;
  elements.quizRecord.textContent = `本次 ${percent}%`;
  try {
    await api("/api/quiz/attempt", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ score, total: state.quiz.questions.length }),
    });
    await loadStats();
  } catch {
    // The score is already visible; persistence can be retried with a future quiz.
  }
}

function formatDuration(seconds) {
  if (seconds < 3600) return `${Math.round(seconds / 60)} 分钟`;
  return `${(seconds / 3600).toFixed(seconds >= 36000 ? 0 : 1)} 小时`;
}

function encouragementFor(seconds) {
  const hours = seconds / 3600;
  if (hours < 0.17) return "先从十分钟开始，知识会在安静里慢慢清晰。";
  if (hours < 0.5) return "你已经进入状态了，保持舒缓的节奏。";
  if (hours < 1) return "半小时的专注正在累积成扎实的理解。";
  if (hours < 2) return "今天的投入很有分量，记得给大脑一点留白。";
  if (hours < 3) return "两小时目标已完成。稳定，比勉强坚持更重要。";
  return "你今天走了很远。现在休息，也是在帮助知识沉淀。";
}

function renderStudyTime() {
  const todaySeconds = state.study.today_seconds + state.pendingSeconds;
  const hours = todaySeconds / 3600;
  elements.todayHours.innerHTML = `今天你已经学习 <strong>${hours.toFixed(1)}</strong> 小时`;
  elements.encouragement.textContent = encouragementFor(todaySeconds);
  elements.focusProgress.style.width = `${Math.min(100, (todaySeconds / 7200) * 100)}%`;
  elements.totalLearning.textContent = formatDuration(state.study.total_seconds + state.pendingSeconds);
  elements.profileToday.textContent = formatDuration(todaySeconds);
  elements.activeDays.textContent = `${state.study.active_days || 0} 天`;
  renderHistory();
}

function lastSevenDays() {
  const days = [];
  for (let offset = 6; offset >= 0; offset -= 1) {
    const date = new Date();
    date.setHours(12, 0, 0, 0);
    date.setDate(date.getDate() - offset);
    days.push({ date: localDate(date), label: `${date.getMonth() + 1}/${date.getDate()}` });
  }
  return days;
}

function renderHistory() {
  const values = new Map((state.study.recent || []).map((day) => [day.date, day.seconds]));
  values.set(localDate(), (values.get(localDate()) || state.study.today_seconds) + state.pendingSeconds);
  const days = lastSevenDays().map((day) => ({ ...day, seconds: values.get(day.date) || 0 }));
  const maximum = Math.max(1800, ...days.map((day) => day.seconds));
  elements.historyChart.innerHTML = days
    .map(
      (day) => `
        <div class="history-day" title="${day.label} · ${formatDuration(day.seconds)}">
          <span>${day.seconds ? Math.round(day.seconds / 60) + "m" : "—"}</span>
          <div class="history-bar-wrap"><i class="history-bar" style="height:${Math.max(day.seconds ? 3 : 0, (day.seconds / maximum) * 100)}%"></i></div>
          <span>${day.label}</span>
        </div>`,
    )
    .join("");
}

async function loadStudyTime() {
  state.study = await api(`/api/study-time?date=${localDate()}`);
  renderStudyTime();
}

async function flushStudyTime() {
  const seconds = Math.floor(state.pendingSeconds);
  if (seconds < 1) return;
  state.pendingSeconds -= seconds;
  try {
    await api("/api/study-time", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ date: localDate(), seconds }),
    });
    state.study.today_seconds += seconds;
    state.study.total_seconds += seconds;
    if (state.study.active_days === 0) state.study.active_days = 1;
  } catch {
    state.pendingSeconds += seconds;
  }
  renderStudyTime();
}

function startStudyTimer() {
  if (state.timerStarted) return;
  state.timerStarted = true;
  const activity = () => { state.lastActivity = Date.now(); };
  ["pointerdown", "keydown", "scroll", "touchstart"].forEach((event) =>
    window.addEventListener(event, activity, { passive: true }),
  );
  setInterval(() => {
    if (document.visibilityState === "visible" && Date.now() - state.lastActivity < 5 * 60 * 1000) {
      state.pendingSeconds += 1;
      if (state.pendingSeconds % 5 === 0) renderStudyTime();
    }
  }, 1000);
  setInterval(flushStudyTime, 30000);
  window.addEventListener("pagehide", () => {
    const seconds = Math.floor(state.pendingSeconds);
    if (seconds < 1) return;
    state.pendingSeconds -= seconds;
    navigator.sendBeacon(
      "/api/study-time",
      new Blob([JSON.stringify({ date: localDate(), seconds })], { type: "application/json" }),
    );
  });
}

async function loadStats() {
  const stats = await api("/api/stats");
  elements.footerStats.textContent = `${stats.chunk_count || 0} 个知识片段`;
  if (stats.quiz_attempt_count) {
    elements.quizRecord.textContent = `平均 ${Math.round(stats.quiz_average)}%`;
  }
}

function openProfile() {
  elements.profileModal.classList.remove("hidden");
  document.body.style.overflow = "hidden";
  renderStudyTime();
}

function closeProfile() {
  elements.profileModal.classList.add("hidden");
  document.body.style.overflow = "";
}

elements.fileInput.addEventListener("change", (event) => uploadFiles(event.target.files));
["dragenter", "dragover"].forEach((event) =>
  elements.uploadZone.addEventListener(event, (value) => {
    value.preventDefault();
    elements.uploadZone.classList.add("dragging");
  }),
);
["dragleave", "drop"].forEach((event) =>
  elements.uploadZone.addEventListener(event, (value) => {
    value.preventDefault();
    elements.uploadZone.classList.remove("dragging");
    if (event === "drop") uploadFiles(value.dataTransfer.files);
  }),
);
elements.selectAll.addEventListener("click", () => {
  const allSelected = state.selectedIds.size === state.documents.length;
  state.selectedIds = new Set(allSelected ? [] : state.documents.map((document) => document.id));
  renderDocuments();
});
elements.chatForm.addEventListener("submit", (event) => {
  event.preventDefault();
  askQuestion(elements.questionInput.value);
});
elements.questionInput.addEventListener("keydown", (event) => {
  if (event.key === "Enter" && !event.shiftKey) {
    event.preventDefault();
    elements.chatForm.requestSubmit();
  }
});
document.querySelectorAll("[data-question]").forEach((button) =>
  button.addEventListener("click", () => askQuestion(button.dataset.question)),
);
elements.quizForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  const submit = elements.quizForm.querySelector("button");
  submit.disabled = true;
  submit.textContent = "正在生成 ···";
  try {
    state.quiz = await api("/api/quiz", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        topic: elements.quizTopic.value.trim(),
        count: Number(elements.quizCount.value),
        document_ids: selectedDocumentIds(),
      }),
    });
    renderQuiz();
  } catch (error) {
    toast(error.message);
  } finally {
    submit.disabled = false;
    submit.textContent = "生成练习";
  }
});

$("#profileTrigger").addEventListener("click", openProfile);
$("#openProfile").addEventListener("click", openProfile);
$("#closeProfile").addEventListener("click", closeProfile);
elements.profileModal.addEventListener("click", (event) => {
  if (event.target === elements.profileModal) closeProfile();
});
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape") closeProfile();
});
document.querySelectorAll("[data-minutes]").forEach((button) => {
  button.addEventListener("click", async () => {
    const seconds = Number(button.dataset.minutes) * 60;
    button.disabled = true;
    try {
      await api("/api/study-time", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ date: localDate(), seconds }),
      });
      await loadStudyTime();
      toast(`已补记 ${button.textContent.replace("+", "").trim()}`);
    } catch (error) {
      toast(error.message);
    } finally {
      button.disabled = false;
    }
  });
});

async function initialize() {
  const results = await Promise.allSettled([loadHealth(), loadDocuments(), loadStats(), loadStudyTime()]);
  const failed = results.find((result) => result.status === "rejected");
  if (failed) toast(failed.reason?.message || "部分数据加载失败");
  startStudyTimer();
}

initialize();

