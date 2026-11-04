// Package scorereports holds the SQL fragments and row fields that decide which runs the boards show (= the constants and fields() of
// server/score_reports.py). The operator and website operations (report, queue, moderate) live with the ops API, which owns its error type.
package scorereports

import "fh2mp/internal/db"

// Fragments, aliased on score_submissions as `s`.
const (
	Threshold    = 10
	Visible      = "NOT EXISTS (SELECT 1 FROM hidden_scores hs WHERE hs.submission_id = s.id)"
	Counts       = "(SELECT COUNT(*) FROM score_reports sr WHERE sr.submission_id = s.id AND sr.dismissed = 0)"
	OwnPending   = "(s.xuid = ? AND EXISTS (SELECT 1 FROM hidden_scores hs WHERE hs.submission_id = s.id AND hs.source = 'pending'))"
	ReviewBoards = "(1,3,6,9)"
	// GhostFlag is a run the ghost rules flagged (not hard: those are hidden) and nobody has reviewed.
	GhostFlag = "EXISTS (SELECT 1 FROM score_verdicts gv WHERE gv.submission_id = s.id AND gv.verdict = 'flag' AND gv.reviewed = 0)"
)

// VisibleTo is Visible plus the viewer's own run that is waiting for its ghost: the game asks for its best right after submitting, before
// the upload. viewer nil = nobody in particular.
func VisibleTo(viewer *int64) (string, []any) {
	if viewer != nil {
		return "(" + Visible + " OR " + OwnPending + ")", []any{*viewer}
	}
	return Visible, nil
}

// Flags are the report / ghost-flag fields every board row carries.
type Flags struct {
	SubmissionID int64
	Reports      int64
	Suspicious   bool
	FlaggedBy    *string // "reports", "ghost", "reports and ghost" or nil
}

// FieldsOf reads the flags from a row of score_submissions (report_count / ghost_flag columns are optional).
func FieldsOf(r *db.Row) Flags {
	count := int64(0)
	if r.Has("report_count") {
		count = r.Int("report_count")
	}
	ghost := false
	if r.Has("ghost_flag") {
		ghost = r.Int("ghost_flag") != 0
	}
	f := Flags{SubmissionID: r.Int("id"), Reports: count, Suspicious: count >= Threshold || ghost}
	var by string
	switch {
	case ghost && count >= Threshold:
		by = "reports and ghost"
	case ghost:
		by = "ghost"
	case count >= Threshold:
		by = "reports"
	default:
		return f
	}
	f.FlaggedBy = &by
	return f
}
