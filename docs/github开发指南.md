# Repo 多仓库开发与 Git 管理

## 1. 目录和职责

工作区：

```text
~/vela_competition/contest/
├── .repo/                                      # repo 元数据，不提交
├── nuttx/ apps/ packages/ vendor/              # openvela 公共仓库
└── contest2026_264_VelaSightsuixingAIzhinengyanjing/
                                                   # 比赛仓，唯一提交仓库
```

核心原则：

- `repo` 负责组织多个 Git 仓库；Git 负责版本管理。
- BK7258 源码只维护在比赛仓 `board/beken/` 下。
- `nuttx/`、`apps/`、`packages/` 和公共 `vendor/` 默认不修改、不提交。
- `vendor/beken/` 下的 BK7258 目录是 manifest 创建的构建入口，不是源码提交位置。

## 2. 初始化工作区

在新的工作区根目录执行，不要在比赛仓子目录中执行 `repo init`：

```bash
mkdir -p ~/vela_competition/contest
cd ~/vela_competition/contest

repo init \
  -u https://github.com/open-vela/contest2026_264_VelaSightsuixingAIzhinengyanjing \
  -b dev-ai-contest-2026 \
  -m contest2026_264_VelaSightsuixingAIzhinengyanjing.xml

repo sync -c -j8
repo status
```

比赛仓路径：

```text
~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing/
```

## 3. 使用开发分支

```bash
cd ~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing
git switch dev-ai-contest-2026
git branch --show-current
git status --short --branch
```

日常开发统一使用 `dev-ai-contest-2026` 分支。

禁止以下做法：

- 不要从 `dev-ai-contest-2026` 创建 feature 分支再发起 PR。
- 不要使用 `feature/xxx` 等分支发起 PR。
- 不要在 fork 仓库维护多个开发分支。

统一分支的原因：

- 仓库只允许 `Rebase and merge`，多个分支或重复历史会导致 rebase 冲突。
- 使用单一分支可以让 PR 始终建立在目标分支之上。
- 减少 merge commit 和重复提交，避免历史混乱。
- 评委和协作者只需关注一个分支。

日常操作：

```bash
cd ~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing

# 切换到开发分支
git switch dev-ai-contest-2026

# 开始工作前，必须按第 3.1 节的完整流程同步 upstream 和 fork
git fetch --all --prune
git status --short --branch
git rebase openvela/dev-ai-contest-2026

# 提交修改
git add <files>
git commit -m "<commit message>"

# 推送前，必须按第 3.2 节验证并更新 fork
git rev-list --left-right --count openvela/dev-ai-contest-2026...HEAD
git push --force-with-lease=refs/heads/dev-ai-contest-2026:<fetch前记录的fork旧commit> \
  fork dev-ai-contest-2026
```

如果 fork 分支历史已经混乱，参照第 9 节的方法整理分支历史，不要新建分支绕过问题。

### 3.1 开发前必须同步上游

每次开始新的开发工作前，必须先确认本地修改已提交或已安全暂存，再同时获取 upstream 和 fork 的最新引用：

```bash
git fetch --all --prune
git status --short --branch
git rev-parse --verify openvela/dev-ai-contest-2026
git rev-parse --verify fork/dev-ai-contest-2026
```

如果工作区有本次任务的未提交修改，不能直接 rebase。优先先提交；如果修改尚未完成，使用带说明的 stash 保存：

```bash
git stash push -m "wip: <简短说明>"
git rebase openvela/dev-ai-contest-2026
git stash pop
```

然后检查当前分支和 upstream 的关系：

```bash
git rev-list --left-right --count openvela/dev-ai-contest-2026...HEAD
```

左值不为 `0` 时必须先 rebase。同步 upstream 只能使用：

```bash
git rebase openvela/dev-ai-contest-2026
```

禁止把 fork 当作 upstream，也禁止使用 `git pull`、`git pull --ff-only` 或
`git merge fork/dev-ai-contest-2026` 同步。fork 只是推送目标，upstream 才是 PR 的目标基线。

禁止以下做法：

- 不要在旧的上游基线上直接开发，否则会与上游新增提交形成分叉。
- 不要使用 `git merge openvela/dev-ai-contest-2026` 同步上游，这会产生 merge commit，导致 GitHub 的 `Rebase and merge` 无法使用。
- 不要使用 `git pull` 或 `git pull --ff-only` 替代 rebase，因为它们只同步 fork 远端，不同步上游。

如果 rebase 时出现冲突，说明上游可能已经包含了相同修改。此时应：

```bash
# 查看冲突文件
git status

# 如果上游已经包含相同功能，选择上游版本
git checkout --theirs <冲突文件>
git add <冲突文件>

# 跳过当前重复提交
git rebase --skip

# 如果是真正的新增修改，解决冲突后继续
git add <冲突文件>
git rebase --continue
```

rebase 完成后，如果 fork 远端有旧历史，需要强制推送：

```bash
git push --force-with-lease=refs/heads/dev-ai-contest-2026:<fetch前记录的fork旧commit> \
  fork dev-ai-contest-2026
```

如果 rebase 过程中提示 `skipped previously applied commit`，说明该提交的补丁已经
包含在 upstream 中，不要使用 `--reapply-cherry-picks`，继续完成 rebase 即可。

如果 GitHub 显示 `This branch has conflicts that must be resolved`，且提示
`Discard 1 commit to make this branch match the upstream repository`，通常是 fork
中存在一个与 upstream 内容相同但 commit hash 不同的重复提交。不要在网页上点击
`Discard`，也不要创建 feature 分支。执行第 9.2 节的历史重建流程，或在确认是重复
提交后执行：

```bash
git rebase --skip
```

### 3.2 推送前必须验证分支状态

推送前必须确认工作区、暂存区、提交历史和 fork 远端都正确。以下命令必须按顺序执行：

```bash
git status --short --branch
git diff --check
git diff --cached --name-only
git rev-list --left-right --count openvela/dev-ai-contest-2026...HEAD
git merge-base --is-ancestor openvela/dev-ai-contest-2026 HEAD
git fetch fork
git rev-parse fork/dev-ai-contest-2026
```

期望输出：

```text
0  N
```

含义：

- 左边 `0`：不落后上游，没有缺少上游提交。
- 右边 `N`：有 N 个新增提交。

如果左边不为 0，说明还没同步上游，必须先 rebase。

如果 `git merge-base --is-ancestor` 返回非零，当前分支不是 upstream 之上的线性历史，
必须停止推送并按第 9 节重建。

如果 N 过大或包含与上游重复的提交，应检查并清理历史，只保留真正新增的提交。
确认暂存文件只来自比赛仓后，再使用带 lease 的强制推送：

```bash
git push --force-with-lease=refs/heads/dev-ai-contest-2026:<fetch前记录的fork旧commit> \
  fork dev-ai-contest-2026
```

推送后必须重新获取 fork 并确认本地与 fork 完全一致：

```bash
git fetch fork
git rev-list --left-right --count fork/dev-ai-contest-2026...HEAD
```

期望输出为 `0 0`。如果不是 `0 0`，不得继续创建或更新 PR。

## 4. 源码位置和 manifest 映射

当前 BK7258 映射为：

```xml
<linkfile src="board/beken/chips/bk7258"
          dest="vendor/beken/chips/bk7258"/>
<linkfile src="board/beken/boards/bk7258"
          dest="vendor/beken/boards/bk7258"/>
```

对应关系：

```text
比赛仓/board/beken/chips/bk7258/
    -> 工作区/vendor/beken/chips/bk7258/

比赛仓/board/beken/boards/bk7258/
    -> 工作区/vendor/beken/boards/bk7258/
```

源码应放在：

```text
contest2026_264_VelaSightsuixingAIzhinengyanjing/board/beken/
```

不要直接把代码放到：

```text
contest/vendor/beken/
contest/nuttx/
contest/apps/
contest/packages/
```

如果新增源码目录，必须同时在比赛仓 manifest 中增加对应的 `<linkfile>`，然后同步并检查映射：

```bash
cd ~/vela_competition/contest
repo sync -c
ls -ld vendor/beken/chips/bk7258
readlink vendor/beken/chips/bk7258
```

## 5. 正确提交

```bash
cd ~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing

git status --short
git diff --check

git add board/beken
git add contest2026_264_VelaSightsuixingAIzhinengyanjing.xml
git add docs/github开发指南.md

git diff --cached --name-only
git commit -m "feat: complete BK7258 porting milestone"

git status --short --branch
git log --oneline -5
```

暂存内容只能来自比赛仓。提交中不应出现：

```text
nuttx/
apps/
packages/
vendor/
.repo/
cmake_out/
out/
```

提交到远端：

```bash
git push origin dev-ai-contest-2026
```

## 6. 检查公共仓库

```bash
cd ~/vela_competition/contest
repo status
git -C nuttx status
git -C vendor/beken status
git -C apps status
```

公共仓库出现修改时：

- BK7258 移植代码放回比赛仓 `board/beken/`。
- openvela 公共功能在对应公共仓库单独建分支、提交和发 PR。
- 不要复制公共仓库文件到比赛仓来掩盖公共仓修改。

## 7. 同步上游

### 7.1 公共仓库同步

同步前先检查：

```bash
cd ~/vela_competition/contest
repo status
```

同步：

```bash
repo sync -c -j8
```

不要在未确认本地修改可丢弃时执行：

```bash
git reset --hard
git checkout -- .
repo sync --force-sync
```

### 7.2 比赛仓同步上游

比赛仓的 fork 分支必须定期与上游 `open-vela` 保持同步。同步时只能使用 rebase，不能使用 merge：

```bash
cd ~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing

# 获取上游最新
git fetch openvela

# 确认工作区干净
git status --short

# rebase 到上游最新
git rebase openvela/dev-ai-contest-2026

# 如果有冲突，按第 3.1 节处理

# 推送到 fork
git push --force-with-lease fork dev-ai-contest-2026
```

禁止以下做法：

- 不要使用 `git merge openvela/dev-ai-contest-2026`，会产生 merge commit，导致 GitHub 的 `Rebase and merge` 失败。
- 不要在 fork 网页上点击 `Sync fork` 后直接使用 `Merge` 方式同步。
- 不要在 fork 中保留与上游内容重复的提交。

## 8. 标签和日志

阶段完成后可在比赛仓创建标签：

```bash
git tag -a bk7258-startup -m "BK7258 startup baseline"
git tag
```

AI Coding 日志放在：

```text
contest2026_264_VelaSightsuixingAIzhinengyanjing/logs/
```

不要把日志放到 `.repo/`，也不要提交构建临时日志。

## 9. Fork PR 与分支历史整理

本仓库只允许 `Rebase and merge`。如果 PR 分支包含 merge commit、重复历史或与目标分支分叉，GitHub 会提示：

```text
This branch cannot be rebased due to conflicts
```

这通常不是代码内容冲突，而是提交历史结构导致无法逐个 rebase。

### 9.1 常见原因

- fork 分支和 upstream 目标分支各自有一套内容相同但提交不同的历史。
- 网页解决冲突时产生了 merge commit。
- PR 分支长期未与目标分支同步。
- 使用过多个分支（如 feature 分支）发起 PR。

### 9.2 解决方法

将 fork 分支重建为目标分支之上只保留有效新增提交：

```bash
cd ~/vela_competition/contest/contest2026_264_VelaSightsuixingAIzhinengyanjing

# 确认远端最新
git fetch --all --prune

# 确认工作树干净
git status --short --branch

# 创建本地备份分支，保存当前完整历史
git branch backup/dev-ai-contest-2026-before-rebase HEAD

# 重置到目标分支最新提交
git reset --hard openvela/dev-ai-contest-2026

# 挑选需要保留的有效提交
git cherry-pick <commit-hash>

# 确认重建前后文件内容一致
test "$(git rev-parse backup/dev-ai-contest-2026-before-rebase^{tree})" = "$(git rev-parse HEAD^{tree})"

# 使用带 lease 的强制推送更新 fork 分支。旧 commit 必须来自本次
# git fetch --all --prune 之后记录的 fork/dev-ai-contest-2026。
git push --force-with-lease=refs/heads/dev-ai-contest-2026:<fetch前记录的fork旧commit> \
  fork dev-ai-contest-2026
```

推送后 PR 会自动更新为 upstream 之上的线性提交序列，`Rebase and merge` 即可使用。

### 9.3 注意事项

- `--force-with-lease` 会检查远端分支是否仍为预期旧版本，比直接 `--force` 更安全。
- 重写历史前必须创建备份分支。
- 重写后必须比较 tree 哈希，确认代码内容没有丢失。
- 旧 PR 会因为源分支重写而自动关闭或失效，需要新建 PR。
- 不要在网页上反复 `Accept incoming change`，如果代码内容本身没有冲突，应先整理分支历史。
- 统一使用 `dev-ai-contest-2026` 分支开发和发起 PR，不要使用 feature 分支。

### 9.4 验证 PR 是否可以合入

```bash
# 确认目标分支是 PR 分支的祖先
git merge-base --is-ancestor openvela/dev-ai-contest-2026 dev-ai-contest-2026

# 确认只有基于 upstream 的新增提交
git rev-list --left-right --count openvela/dev-ai-contest-2026...dev-ai-contest-2026
# 预期输出：0 N，N 为本地真正新增的提交数
```

如果输出为 `0 N`，且 `N` 只包含本次真正新增的提交，可以直接创建或更新 PR。
如果左值不为 `0`、出现 merge commit 或 N 包含重复提交，必须先整理历史，不能直接推送。

## 10. 最终原则

- 比赛仓是参赛代码的唯一提交仓库。
- Git 管理比赛仓源码和 manifest；manifest 管理工作区映射。
- 软链接或 linkfile 只是构建入口，不是另一份源码。
- 每次提交前检查 `git diff --cached --name-only`。
- 每个阶段完成后检查 `repo status`。
- 公共仓库需要修改时，单独管理、单独提交、单独发 PR。
- 统一使用 `dev-ai-contest-2026` 分支开发，不使用 feature 分支。
- 每次开发前必须 `git fetch openvela` 并 `git rebase openvela/dev-ai-contest-2026`，确保基于上游最新提交开发。
- 禁止使用 `git merge` 同步上游，只能使用 `git rebase`，避免产生 merge commit 导致 `Rebase and merge` 失败。
- 不要在 fork 中保留与上游内容重复的提交；rebase 时遇到重复提交应使用 `git rebase --skip` 跳过。
- 推送前必须验证 `git rev-list --left-right --count openvela/dev-ai-contest-2026...HEAD` 输出为 `0 N`，确保不落后上游。
- 每次推送前必须 `git fetch --all --prune`，记录 fork 旧 commit，并使用带明确 lease 的 `git push --force-with-lease`。
- 推送后必须验证 `git rev-list --left-right --count fork/dev-ai-contest-2026...HEAD` 输出为 `0 0`。
- GitHub 提示 `Discard 1 commit` 时，先判断是否为重复提交，禁止直接点击网页 `Discard` 或创建 feature 分支绕过。
