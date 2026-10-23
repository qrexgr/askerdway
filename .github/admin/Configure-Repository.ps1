[CmdletBinding()]
param(
    [switch]$Apply,
    [switch]$EnableBranchProtection
)
$ErrorActionPreference = 'Stop'
$repository = 'qrexgr/askerdway'

function Invoke-GitHubApi {
    param([string]$Path, [string]$Method = 'GET', $Payload = $null)
    $arguments = @('api', $Path, '--method', $Method)
    if ($null -ne $Payload) {
        $json = $Payload | ConvertTo-Json -Depth 30 -Compress
        $response = $json | & gh @arguments --input -
    } else {
        $response = & gh @arguments
    }
    if ($LASTEXITCODE -ne 0) { throw "GitHub API failed: $Method $Path" }
    if ($response) { return (($response -join "`n") | ConvertFrom-Json) }
}

$login = (Invoke-GitHubApi 'user').login
$repo = Invoke-GitHubApi "repos/$repository"
if ($login -ne 'qrexgr' -or -not $repo.permissions.admin) {
    throw 'Run as repository owner qrexgr with administrative access.'
}
if (-not $Apply) {
    [pscustomobject]@{
        Repository = $repository
        Visibility = $repo.visibility
        DefaultBranch = $repo.default_branch
        Mode = 'Preview only. Use -Apply to apply baseline settings.'
        Protection = 'Use -Apply -EnableBranchProtection after public visibility or a plan upgrade.'
    } | Format-List
    return
}

$null = Invoke-GitHubApi "repos/$repository" 'PATCH' @{
    allow_auto_merge = $false
    allow_merge_commit = $false
    allow_rebase_merge = $false
    allow_squash_merge = $true
    delete_branch_on_merge = $true
    allow_update_branch = $true
    has_issues = $true
    has_wiki = $false
    squash_merge_commit_title = 'PR_TITLE'
    squash_merge_commit_message = 'PR_BODY'
}
$null = Invoke-GitHubApi "repos/$repository/actions/permissions" 'PUT' @{
    enabled = $true
    allowed_actions = 'selected'
    sha_pinning_required = $true
}
$null = Invoke-GitHubApi "repos/$repository/actions/permissions/selected-actions" 'PUT' @{
    github_owned_allowed = $true
    verified_allowed = $false
    patterns_allowed = @()
}
$null = Invoke-GitHubApi "repos/$repository/actions/permissions/workflow" 'PUT' @{
    default_workflow_permissions = 'read'
    can_approve_pull_request_reviews = $false
}
if ($repo.private) {
    $null = Invoke-GitHubApi "repos/$repository/actions/permissions/fork-pr-workflows-private-repos" 'PUT' @{
        run_workflows_from_fork_pull_requests = $false
        send_write_tokens_to_workflows = $false
        send_secrets_and_variables = $false
        require_approval_for_fork_pr_workflows = $true
    }
} else {
    $null = Invoke-GitHubApi "repos/$repository/actions/permissions/fork-pr-contributor-approval" 'PUT' @{
        approval_policy = 'all_external_contributors'
    }
    $null = Invoke-GitHubApi "repos/$repository/private-vulnerability-reporting" 'PUT'
    $null = Invoke-GitHubApi "repos/$repository" 'PATCH' @{
        security_and_analysis = @{
            secret_scanning = @{ status = 'enabled' }
            secret_scanning_push_protection = @{ status = 'enabled' }
        }
    }
}
$null = Invoke-GitHubApi "repos/$repository/vulnerability-alerts" 'PUT'
Write-Output 'Baseline settings applied. Repository visibility was not changed.'

if (-not $EnableBranchProtection) {
    Write-Output 'Branch and tag protection not applied. Use -EnableBranchProtection when supported.'
    return
}

# Admins may bypass owner review only through a PR. Keep owner as the sole admin.
$collaborators = @(Invoke-GitHubApi "repos/$repository/collaborators?per_page=100")
if (@($collaborators | Where-Object { $_.permissions.admin -and $_.login -ne 'qrexgr' }).Count) {
    throw 'Another administrator could bypass owner review. Review access before applying rulesets.'
}
$checks = Invoke-GitHubApi "repos/$repository/commits/$($repo.default_branch)/check-runs?per_page=100"
foreach ($name in @('Repository hygiene', 'Rust checks')) {
    if (-not @($checks.check_runs | Where-Object {
        $_.name -eq $name -and $_.conclusion -eq 'success' -and $_.app.id -eq 15368
    }).Count) {
        throw "Run successful Pull request checks on the current default branch before requiring: $name"
    }
}
# GitHub returns 403 here for a private repository without a qualifying plan.
# Stop on that response; do not create disabled rules or claim protection is active.
$rulesets = @(Invoke-GitHubApi "repos/$repository/rulesets?per_page=100")
foreach ($file in @('branch-safety.json', 'owner-review.json', 'tag-safety.json', 'tag-creation.json')) {
    $rule = Get-Content -LiteralPath (Join-Path $PSScriptRoot $file) -Raw | ConvertFrom-Json
    $matches = @($rulesets | Where-Object { $_.name -eq $rule.name })
    if ($matches.Count -gt 1) { throw "Duplicate ruleset name: $($rule.name)" }
    if ($matches.Count -eq 1) {
        $null = Invoke-GitHubApi "repos/$repository/rulesets/$($matches[0].id)" 'PUT' $rule
    } else {
        $null = Invoke-GitHubApi "repos/$repository/rulesets" 'POST' $rule
    }
    Write-Output "Active ruleset applied: $($rule.name)"
}
Write-Output 'Read back Settings > Rules > Rulesets and confirm all four are Active.'
