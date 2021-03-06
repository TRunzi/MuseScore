//=============================================================================
//  MuseScore
//  Music Composition & Notation
//
//  Copyright (C) 2018 Werner Schweer
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2
//  as published by the Free Software Foundation and appearing in
//  the file LICENCE.GPL
//=============================================================================

#include "score.h"
#include "page.h"
#include "system.h"
#include "measure.h"
#include "layout.h"
#include "bracket.h"
#include "spanner.h"
#include "barline.h"
#include "tie.h"
#include "chord.h"
#include "staff.h"
#include "box.h"
#include "spacer.h"
#include "sym.h"
#include "systemdivider.h"
#include "tuplet.h"
#include "dynamic.h"
#include "stafflines.h"
#include "tempotext.h"
#include "hairpin.h"
#include "part.h"
#include "keysig.h"
#include "sig.h"
#include "breath.h"
#include "tempo.h"
#include "fermata.h"
#include "lyrics.h"

namespace Ms {

extern bool isTopBeam(ChordRest* cr);
extern bool notTopBeam(ChordRest* cr);
void processLines(System* system, std::vector<Spanner*> lines, bool align);
void layoutTies(Chord* ch, System* system, int stick);
void layoutDrumsetChord(Chord* c, const Drumset* drumset, StaffType* st, qreal spatium);

//---------------------------------------------------------
//   layoutLinear
//    in linear mode there is only one page
//    which contains one system
//---------------------------------------------------------

void Score::layoutLinear(bool layoutAll, LayoutContext& lc)
      {
      Page* page;
      System* system;

//      if (layoutAll) {
      if (true) {
            qDeleteAll(systems());
            systems().clear();
            qDeleteAll(pages());
            pages().clear();

            page = new Page(this);
            pages().push_back(page);
            page->bbox().setRect(0.0, 0.0, loWidth(), loHeight());
            page->setNo(0);

            system = new System(this);
            systems().append(system);
            page->appendSystem(system);
            for (int i = 0; i < nstaves(); ++i)
                  system->insertStaff(i);
            }
      else {
            page = pages().front();
            system = systems().front();
            }

      lc.page = page;
      lc.layoutLinear();
      }

//---------------------------------------------------------
//   layoutLinear
//---------------------------------------------------------

void LayoutContext::layoutLinear()
      {
      System* system = score->systems().front();

      QPointF pos;
      bool firstMeasure = true;
      for (MeasureBase* mb = score->first(); mb; mb = mb->next()) {
            qreal ww = 0.0;
            if (mb->isVBox())
                  continue;
            system->appendMeasure(mb);
            layoutMeasureLinear(mb);
            if (mb->isMeasure()) {
                  Measure* m = toMeasure(mb);
                  if (firstMeasure) {
                        system->layoutSystem(0.0);
                        if (m->repeatStart()) {
                              Segment* s = m->findSegmentR(SegmentType::StartRepeatBarLine, 0);
                              if (!s->enabled())
                                    s->setEnabled(true);
                              }
                        m->addSystemHeader(true);
                        pos.rx() += system->leftMargin();
                        firstMeasure = false;
                        }
                  else if (m->header())
                        m->removeSystemHeader();
                  m->createEndBarLines(true);
                  m->computeMinWidth();
                  ww = m->width();
                  m->stretchMeasure(ww);
                  m->setPos(pos);
                  m->layoutStaffLines();
                  }
            else if (mb->isHBox()) {
                  mb->setPos(pos + QPointF(toHBox(mb)->topGap(), 0.0));
                  mb->layout();
                  ww = mb->width();;
                  }
            pos.rx() += ww;
            }
      system->setWidth(pos.x());
      page->setWidth(system->width());

      score->hideEmptyStaves(system, true);

      //
      // compute measure shape
      //

      for (int si = 0; si < score->nstaves(); ++si) {
            for (MeasureBase* mb : system->measures()) {
                  if (!mb->isMeasure())
                        continue;
                  Measure* m = toMeasure(mb);
                  Shape& ss  = m->staffShape(si);
                  ss.clear();

                  for (Segment& s : m->segments()) {
                        if (s.isTimeSigType())       // hack: ignore time signatures
                              continue;
                        ss.add(s.staffShape(si).translated(s.pos()));
                        }
                  ss.add(m->staffLines(si)->bbox());
                  }
            }
      //
      // layout
      //    - beams
      //    - RehearsalMark, StaffText
      //    - Dynamic
      //    - update the segment shape + measure shape
      //
      //
      int stick = system->measures().front()->tick();
      int etick = system->measures().back()->endTick();

      //
      // layout slurs
      //
      if (etick > stick) {    // ignore vbox
            auto spanners = score->spannerMap().findOverlapping(stick, etick);

            std::vector<Spanner*> spanner;
            for (auto interval : spanners) {
                  Spanner* sp = interval.value;
                  if (sp->tick() < etick && sp->tick2() >= stick) {
                        if (sp->isSlur())
                              spanner.push_back(sp);
                        }
                  }
            processLines(system, spanner, false);
            }

      std::vector<Dynamic*> dynamics;
      for (MeasureBase* mb : system->measures()) {
            if (!mb->isMeasure())
                  continue;
            SegmentType st = SegmentType::ChordRest;
            Measure* m = toMeasure(mb);
            for (Segment* s = m->first(st); s; s = s->next(st)) {
                  for (Element* e : s->elist()) {
                        if (!e)
                              continue;
                        if (e->isChordRest()) {
                              ChordRest* cr = toChordRest(e);
                              if (isTopBeam(cr)) {
                                    cr->beam()->layout();
                                    Shape shape(cr->beam()->shape().translated(-(cr->segment()->pos()+mb->pos())));
                                    s->staffShape(cr->staffIdx()).add(shape);
                                    m->staffShape(cr->staffIdx()).add(shape.translated(s->pos()));
                                    }
                              if (e->isChord()) {
                                    Chord* c = toChord(e);
                                    for (Chord* ch : c->graceNotes())
                                          layoutTies(ch, system, stick);
                                    layoutTies(c, system, stick);
                                    c->layoutArticulations2();
                                    }
                              }
                        }
                  for (Element* e : s->annotations()) {
                        if (e->visible() && e->isDynamic()) {
                              Dynamic* d = toDynamic(e);
                              d->layout();

                              if (d->autoplace()) {
                                    // If dynamic is at start or end of a hairpin
                                    // don't autoplace. This is done later on layout of hairpin
                                    // and allows horizontal alignment of dynamic and hairpin.

                                    int tick = d->tick();
                                    auto si = score->spannerMap().findOverlapping(tick, tick);
                                    bool doAutoplace = true;
                                    for (auto is : si) {
                                          Spanner* sp = is.value;
                                          sp->computeStartElement();
                                          sp->computeEndElement();

                                          if (sp->isHairpin()
                                             && (lookupDynamic(sp->startElement()) == d
                                             || lookupDynamic(sp->endElement()) == d))
                                                doAutoplace = false;
                                          }
                                    if (doAutoplace) {
                                          d->doAutoplace();
                                          dynamics.push_back(d);
                                          }
                                    }
                              }
                        else if (e->isFiguredBass())
                              e->layout();
                        }
                  }
            }

      //
      // layout tuplet
      //

      for (MeasureBase* mb : system->measures()) {
            if (!mb->isMeasure())
                  continue;
            Measure* m = toMeasure(mb);
            static const SegmentType st { SegmentType::ChordRest };
            for (int track = 0; track < score->ntracks(); ++track) {
                  if (!score->staff(track / VOICES)->show()) {
                        track += VOICES-1;
                        continue;
                        }
                  for (Segment* s = m->first(st); s; s = s->next(st)) {
                        ChordRest* cr = s->cr(track);
                        if (!cr)
                              continue;
                        DurationElement* de = cr;
                        while (de->tuplet() && de->tuplet()->elements().front() == de) {
                              Tuplet* t = de->tuplet();
                              t->layout();
                              s->staffShape(t->staffIdx()).add(t->shape().translated(-s->pos()));
                              m->staffShape(t->staffIdx()).add(t->shape());
                              de = de->tuplet();
                              }
                        }
                  }
            }

      // add dynamics shape to staff shape
      for (Dynamic* d : dynamics) {
            int si = d->staffIdx();
            Segment* s = d->segment();
            s->staffShape(si).add(d->shape().translated(d->pos()));
            Measure* m = s->measure();
            m->staffShape(si).add(d->shape().translated(s->pos() + d->pos()));
            }

      //
      //    layout SpannerSegments for current system
      //

      if (etick > stick) {    // ignore vbox
            auto spanners = score->spannerMap().findOverlapping(stick, etick);

            std::vector<Spanner*> ottavas;
            std::vector<Spanner*> spanner;
            std::vector<Spanner*> pedal;

            for (auto interval : spanners) {
                  Spanner* sp = interval.value;
                  if (sp->tick() < etick && sp->tick2() > stick) {
                        if (sp->isOttava())
                              ottavas.push_back(sp);
                        else if (sp->isPedal())
                              pedal.push_back(sp);
                        else if (!sp->isSlur())             // slurs are already handled
                              spanner.push_back(sp);
                        }
                  }
            processLines(system, ottavas, false);
            processLines(system, pedal, true);
            processLines(system, spanner, false);

            //
            // vertical align volta segments
            //
            std::vector<SpannerSegment*> voltaSegments;
            for (SpannerSegment* ss : system->spannerSegments()) {
                  if (ss->isVoltaSegment())
                       voltaSegments.push_back(ss);
                 }
            if (voltaSegments.size() > 1) {
                  qreal y = 0;
                  for (SpannerSegment* ss : voltaSegments)
                        y = qMin(y, ss->userOff().y());
                  for (SpannerSegment* ss : voltaSegments)
                        ss->setUserYoffset(y);
                  }
            for (Spanner* sp : score->unmanagedSpanners()) {
                  if (sp->tick() >= etick || sp->tick2() < stick)
                        continue;
                  sp->layout();
                  }

            //
            // add SpannerSegment shapes to staff shapes
            //

            for (MeasureBase* mb : system->measures()) {
                  if (!mb->isMeasure())
                        continue;
                  Measure* m = toMeasure(mb);
                  for (SpannerSegment* ss : system->spannerSegments()) {
                        Spanner* sp = ss->spanner();
                        if (sp->tick() < m->endTick() && sp->tick2() > m->tick()) {
                              // spanner shape must be translated from system coordinate space
                              // to measure coordinate space
                              Shape* shape = &m->staffShape(sp->staffIdx());
                              if (ss->isLyricsLineSegment())
                                    shape->add(ss->shape().translated(-m->pos()));
                              else
                                    shape->add(ss->shape().translated(ss->pos() - m->pos()));
                              }
                        }
                  }
            }

      //
      // TempoText, Fermata
      //

      for (MeasureBase* mb : system->measures()) {
            if (!mb->isMeasure())
                  continue;
            SegmentType st = SegmentType::ChordRest;
            Measure* m = toMeasure(mb);
            for (Segment* s = m->first(st); s; s = s->next(st)) {
                  for (Element* e : s->annotations()) {
                        if (e->isTempoText()) {
                              TempoText* tt = toTempoText(e);
                              score->setTempo(tt->segment(), tt->tempo());
                              tt->layout();
                              }
                        else if (e->isFermata()) {
                              e->layout();
                              int si = e->staffIdx();
                              s->staffShape(si).add(e->shape().translated(e->pos()));
                              m->staffShape(si).add(e->shape().translated(s->pos() + e->pos()));
                              }
                        }
                  }
            }

      //
      // Jump, Marker
      //

      for (MeasureBase* mb : system->measures()) {
            if (!mb->isMeasure())
                  continue;
            Measure* m = toMeasure(mb);
            for (Element* e : m->el()) {
                  if (e->visible() && (e->isJump() || e->isMarker()))
                        e->layout();
                  }
            }

      //
      // RehearsalMark, StaffText, FretDiagram
      //

      for (MeasureBase* mb : system->measures()) {
            if (!mb->isMeasure())
                  continue;
            SegmentType st = SegmentType::ChordRest;
            Measure* m = toMeasure(mb);
            for (Segment* s = m->first(st); s; s = s->next(st)) {
                  // layout in specific order
                  for (Element* e : s->annotations()) {
                        if (e->visible() && e->isFretDiagram())
                              e->layout();
                        }
                  for (Element* e : s->annotations()) {
                        if (e->visible() && (e->isStaffText() || e->isHarmony()))
                              e->layout();
                        }
                  for (Element* e : s->annotations()) {
                        if (e->visible() && e->isRehearsalMark())
                              e->layout();
                        }
                  }
            }

      score->layoutLyrics(system);

      system->layout2();   // compute staff distances

      for (MeasureBase* mb : system->measures()) {
            if (!mb->isMeasure())
                  continue;
            Measure* m = toMeasure(mb);

            for (int track = 0; track < score->ntracks(); ++track) {
                  for (Segment* segment = m->first(); segment; segment = segment->next()) {
                        Element* e = segment->element(track);
                        if (!e)
                              continue;
                        if (e->isChordRest()) {
                              if (!score->staff(track2staff(track))->show())
                                    continue;
                              ChordRest* cr = toChordRest(e);
                              if (notTopBeam(cr))                   // layout cross staff beams
                                    cr->beam()->layout();

                              if (cr->isChord()) {
                                    Chord* c = toChord(cr);
                                    for (Chord* cc : c->graceNotes()) {
                                          if (cc->beam() && cc->beam()->elements().front() == cc)
                                                cc->beam()->layout();
                                          for (Note* n : cc->notes()) {
                                                Tie* tie = n->tieFor();
                                                if (tie)
                                                      tie->layout();
                                                for (Spanner* sp : n->spannerFor())
                                                      sp->layout();
                                                }
                                          for (Element* element : cc->el()) {
                                                if (element->isSlur())
                                                      element->layout();
                                                }
                                          }
                                    c->layoutArpeggio2();
                                    for (Note* n : c->notes()) {
                                          Tie* tie = n->tieFor();
                                          if (tie)
                                                tie->layout();
                                          for (Spanner* sp : n->spannerFor())
                                                sp->layout();
                                          }
                                    }
                              }
                        else if (e->isBarLine())
                              toBarLine(e)->layout2();
                        }
                  }
            m->layout2();
            }
      page->setPos(0, 0);
      system->setPos(page->lm(), page->tm() + score->styleP(Sid::staffUpperBorder));
      page->setWidth(system->width());
      page->rebuildBspTree();
      }

//---------------------------------------------------------
//   layoutMeasureLinear
//---------------------------------------------------------

void LayoutContext::layoutMeasureLinear(MeasureBase* mb)
      {
      adjustMeasureNo(mb);

      if (!mb->isMeasure()) {
            mb->setTick(tick);
            return;
            }

      //-----------------------------------------
      //    process one measure
      //-----------------------------------------

      Measure* measure = toMeasure(mb);
      measure->moveTicks(tick - measure->tick());

      //
      //  implement section break rest
      //
      if (measure->sectionBreak() && measure->pause() != 0.0)
            score->setPause(measure->endTick(), measure->pause());

      //
      // calculate accidentals and note lines,
      // create stem and set stem direction
      //
      for (int staffIdx = 0; staffIdx < score->nstaves(); ++staffIdx) {
            Staff* staff           = score->staff(staffIdx);
            const Drumset* drumset = staff->part()->instrument()->useDrumset() ? staff->part()->instrument()->drumset() : 0;
            AccidentalState as;      // list of already set accidentals for this measure
            as.init(staff->keySigEvent(measure->tick()), staff->clef(measure->tick()));

            for (Segment& segment : measure->segments()) {
                  if (segment.isKeySigType()) {
                        KeySig* ks = toKeySig(segment.element(staffIdx * VOICES));
                        if (!ks)
                              continue;
                        int tick = segment.tick();
                        as.init(staff->keySigEvent(tick), staff->clef(tick));
                        ks->layout();
                        }
                  else if (segment.isChordRestType()) {
                        StaffType* st = staff->staffType(segment.tick());
                        int track     = staffIdx * VOICES;
                        int endTrack  = track + VOICES;

                        for (int t = track; t < endTrack; ++t) {
                              ChordRest* cr = segment.cr(t);
                              if (!cr)
                                    continue;
                              qreal m = staff->mag(segment.tick());
                              if (cr->small())
                                    m *= score->styleD(Sid::smallNoteMag);

                              if (cr->isChord()) {
                                    Chord* chord = toChord(cr);
                                    chord->cmdUpdateNotes(&as);
                                    for (Chord* c : chord->graceNotes()) {
                                          c->setMag(m * score->styleD(Sid::graceNoteMag));
                                          c->computeUp();
                                          if (c->stemDirection() != Direction::AUTO)
                                                c->setUp(c->stemDirection() == Direction::UP);
                                          else
                                                c->setUp(!(t % 2));
                                          if (drumset)
                                                layoutDrumsetChord(c, drumset, st, score->spatium());
                                          c->layoutStem1();
                                          }
                                    if (drumset)
                                          layoutDrumsetChord(chord, drumset, st, score->spatium());
                                    chord->computeUp();
                                    chord->layoutStem1();   // create stems needed to calculate spacing
                                                            // stem direction can change later during beam processing
                                    }
                              cr->setMag(m);
                              }
                        }
                  else if (segment.isClefType()) {
                        Element* e = segment.element(staffIdx * VOICES);
                        if (e) {
                              toClef(e)->setSmall(true);
                              e->layout();
                              }
                        }
                  else if (segment.isType(SegmentType::TimeSig | SegmentType::Ambitus | SegmentType::HeaderClef)) {
                        Element* e = segment.element(staffIdx * VOICES);
                        if (e)
                              e->layout();
                        }
                  }
            }

      score->createBeams(measure);

      for (int staffIdx = 0; staffIdx < score->nstaves(); ++staffIdx) {
            for (Segment& segment : measure->segments()) {
                  if (segment.isChordRestType()) {
                        score->layoutChords1(&segment, staffIdx);
                        for (int voice = 0; voice < VOICES; ++voice) {
                              ChordRest* cr = segment.cr(staffIdx * VOICES + voice);
                              if (cr) {
                                    for (Lyrics* l : cr->lyrics()) {
                                          if (l)
                                                l->layout();
                                          }
                                    if (cr->isChord())
                                          toChord(cr)->layoutArticulations();
                                    }
                              }
                        }
                  }
            }

      for (Segment& segment : measure->segments()) {
            if (segment.isBreathType()) {
                  qreal length = 0.0;
                  int tick = segment.tick();
                  // find longest pause
                  for (int i = 0, n = score->ntracks(); i < n; ++i) {
                        Element* e = segment.element(i);
                        if (e && e->isBreath()) {
                              Breath* b = toBreath(e);
                              b->layout();
                              length = qMax(length, b->pause());
                              }
                        }
                  if (length != 0.0)
                        score->setPause(tick, length);
                  }
            else if (segment.isTimeSigType()) {
                  for (int staffIdx = 0; staffIdx < score->nstaves(); ++staffIdx) {
                        TimeSig* ts = toTimeSig(segment.element(staffIdx * VOICES));
                        if (ts)
                              score->staff(staffIdx)->addTimeSig(ts);
                        }
                  }
            else if (score->isMaster() && segment.isChordRestType()) {
                  for (Element* e : segment.annotations()) {
                        if (!(e->isTempoText()
                           || e->isDynamic()
                           || e->isFermata()
                           || e->isRehearsalMark()
                           || e->isFretDiagram()
                           || e->isHarmony()
                           || e->isStaffText()              // ws: whats left?
                           || e->isFiguredBass())) {
                              e->layout();
                              }
                        }
                  // TODO, this is not going to work, we just cleaned the tempomap
                  // it breaks the test midi/testBaroqueOrnaments.mscx where first note has stretch 2
                  // Also see fixTicks
                  qreal stretch = 0.0;
                  for (Element* e : segment.annotations()) {
                        if (e->isFermata())
                              stretch = qMax(stretch, toFermata(e)->timeStretch());
                        }
                  if (stretch != 0.0 && stretch != 1.0) {
                        qreal otempo = score->tempomap()->tempo(segment.tick());
                        qreal ntempo = otempo / stretch;
                        score->setTempo(segment.tick(), ntempo);
                        int etick = segment.tick() + segment.ticks() - 1;
                        auto e = score->tempomap()->find(etick);
                        if (e == score->tempomap()->end())
                              score->setTempo(etick, otempo);
                        }
                  }
            else if (segment.isChordRestType()) {
                  // chord symbols need to be layouted in parts too
                  for (Element* e : segment.annotations()) {
                        if (e->isHarmony())
                              e->layout();
                        }
                  }
            }

      // update time signature map
      // create event if measure len and time signature are different
      // even if they are equivalent 4/4 vs 2/2
      // also check if nominal time signature has changed

      if (score->isMaster() && ((!measure->len().identical(sig) && measure->len() != sig * measure->mmRestCount())
         || (prevMeasure && prevMeasure->isMeasure()
         && !measure->timesig().identical(toMeasure(prevMeasure)->timesig()))))
            {
            if (measure->isMMRest())
                  sig = measure->mmRestFirst()->len();
            else
                  sig = measure->len();
            score->sigmap()->add(tick, SigEvent(sig, measure->timesig(), measure->no()));
            }

      Segment* seg = measure->findSegmentR(SegmentType::StartRepeatBarLine, 0);
      if (measure->repeatStart()) {
            if (!seg)
                  seg = measure->getSegmentR(SegmentType::StartRepeatBarLine, 0);
            measure->barLinesSetSpan(seg);      // this also creates necessary barlines
            for (int staffIdx = 0; staffIdx < score->nstaves(); ++staffIdx) {
                  BarLine* b = toBarLine(seg->element(staffIdx * VOICES));
                  if (b) {
                        b->setBarLineType(BarLineType::START_REPEAT);
                        b->layout();
                        }
                  }
            }
      else if (seg)
            score->undoRemoveElement(seg);

      for (Segment& s : measure->segments()) {
            // DEBUG: relayout grace notes as beaming/flags may have changed
            if (s.isChordRestType()) {
                  for (Element* e : s.elist()) {
                        if (e && e->isChord()) {
                              Chord* chord = toChord(e);
                              chord->layout();
                              if (chord->tremolo())            // debug
                                    chord->tremolo()->layout();
                              }
                        }
                  }
            else if (s.isEndBarLineType())
                  continue;
            s.createShapes();
            }

      tick += measure->ticks();
      }

} // namespace Ms

