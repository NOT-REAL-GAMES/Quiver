#!/usr/bin/env python3
"""CPU checks of the evidence gate, especially requested-only false positives."""
import unittest
import smoke_radiance_shadow_filter as shadow

class ShadowFilterEvidenceTest(unittest.TestCase):
    def state(self,requested,active,cvar=1):
        return (f'"r_radiance_shadow_filter" is "{cvar}"\n'
                f'Radiance: shadow filter requested={requested} active={active}\n')
    def test_actual_filter_required(self):
        shadow.verify_filter(self.state(1,1),1)
        shadow.verify_filter(self.state(0,0,0),0)
        for text in (self.state(1,0),self.state(0,1),self.state(1,1,0),
                     '"r_radiance_shadow_filter" is "1"\n'):
            with self.subTest(text=text),self.assertRaises(RuntimeError):shadow.verify_filter(text,1)
    def test_invalid_active_strength_cannot_pass(self):
        for value in ('nan','inf','-inf','nonnumeric',2):
            with self.subTest(value=value),self.assertRaises(RuntimeError):
                shadow.verify_filter(self.state(1,value),1)
    def test_strength_is_compared_without_integer_truncation(self):
        shadow.verify_filter(self.state(.75,.75,.75),.75)
        with self.assertRaises(RuntimeError):shadow.verify_filter(self.state(.75,.7,.75),.75)

    def test_filter_windows_require_actual_exclusive_submissions(self):
        initial='Radiance: shadow filter frames active=12 inactive=20'
        final='Radiance: shadow filter frames active=16 inactive=20'
        self.assertEqual(shadow.verify_filter_frames(initial,final,1)['delta'],{'active':4,'inactive':0})
        for wrong in ('Radiance: shadow filter frames active=16 inactive=21',initial,
                      'Radiance: shadow filter frames active=12 inactive=23'):
            with self.subTest(wrong=wrong),self.assertRaises(RuntimeError):shadow.verify_filter_frames(initial,wrong,1)
        final='Radiance: shadow filter frames active=12 inactive=23'
        self.assertEqual(shadow.verify_filter_frames(initial,final,0)['delta'],{'active':0,'inactive':3})
    def test_filter_counter_wrap_and_invalid_values(self):
        before='Radiance: shadow filter frames active=4294967294 inactive=9'
        after='Radiance: shadow filter frames active=3 inactive=9'
        self.assertEqual(shadow.verify_filter_frames(before,after,1)['delta']['active'],5)
        for wrong in ('','Radiance: shadow filter frames active=4294967296 inactive=0'):
            with self.subTest(wrong=wrong),self.assertRaises(RuntimeError):shadow.filter_frames(wrong)

    def test_actual_world_radius_is_required(self):
        good=self.state(1,1).rstrip()+' radius=8 active_radius=8'
        shadow.verify_filter(good,1,8)
        shadow.verify_filter(self.state(0,0,0).rstrip()+' radius=8 active_radius=0',0,8)
        for wrong in (good.replace('active_radius=8','active_radius=4'),good.replace('radius=8','radius=16'),
                      good.replace('active_radius=8','active_radius=nan'),self.state(1,1)):
            with self.subTest(wrong=wrong),self.assertRaises(RuntimeError):shadow.verify_filter(wrong,1,8)

if __name__=='__main__':unittest.main()
