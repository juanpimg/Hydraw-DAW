(function () {
  'use strict';

  /* ─── copy to clipboard ─── */
  document.querySelectorAll('.copy').forEach(function (btn) {
    btn.addEventListener('click', function () {
      var txt = btn.getAttribute('data-copy') || '';
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(txt).catch(function () { fallbackCopy(txt); });
      } else {
        fallbackCopy(txt);
      }
      var orig = btn.textContent;
      btn.textContent = 'copied!';
      btn.classList.add('copied');
      setTimeout(function () {
        btn.textContent = orig;
        btn.classList.remove('copied');
      }, 1200);
    });
  });

  function fallbackCopy(str) {
    var ta = document.createElement('textarea');
    ta.value = str;
    ta.style.position = 'fixed';
    ta.style.left = '-9999px';
    document.body.appendChild(ta);
    ta.select();
    try { document.execCommand('copy'); } catch (e) {}
    document.body.removeChild(ta);
  }

  /* ─── terminal copy on click anywhere in term with data-copy ─── */
  document.querySelectorAll('.term[data-copy]').forEach(function (term) {
    term.addEventListener('click', function (e) {
      if (e.target.closest('.copy')) return;
      var btn = term.querySelector('.copy');
      if (btn) btn.click();
    });
  });

  /* ─── sidebar scrollspy ─── */
  var sections = document.querySelectorAll('.section, .hero-banner');
  var navLinks = document.querySelectorAll('.sidebar-nav a[data-section]');
  var sidebar = document.querySelector('.sidebar');
  var currentSection = '';

  function updateActiveLink() {
    var scrollY = window.scrollY;
    var found = '';
    var offset = 120;

    sections.forEach(function (sec) {
      if (!sec.id) return;
      var top = sec.offsetTop;
      var height = sec.offsetHeight;
      if (scrollY >= top - offset && scrollY < top + height - offset) {
        found = sec.id;
      }
    });

    if (found !== currentSection) {
      currentSection = found;
      navLinks.forEach(function (a) {
        a.classList.toggle('active', a.getAttribute('data-section') === found);
      });
    }
  }

  window.addEventListener('scroll', updateActiveLink, { passive: true });
  window.addEventListener('resize', updateActiveLink, { passive: true });
  updateActiveLink();

  /* ─── smooth click-to-nav with active highlight ─── */
  navLinks.forEach(function (a) {
    a.addEventListener('click', function (e) {
      var href = a.getAttribute('href');
      if (!href || href === '#') return;
      var target = document.querySelector(href);
      if (!target) return;

      e.preventDefault();

      var y = target.getBoundingClientRect().top + window.scrollY - 90;
      window.scrollTo({ top: y, behavior: 'smooth' });

      navLinks.forEach(function (l) { l.classList.remove('active'); });
      a.classList.add('active');
      currentSection = a.getAttribute('data-section') || '';
    });
  });

  /* ─── details / collapse animation ─── */
  document.querySelectorAll('details').forEach(function (d) {
    d.addEventListener('toggle', function () {
      // details element already toggles natively; no extra work needed
    });
  });

  /* ─── section anchor click to copy URL ─── */
  document.querySelectorAll('.section-anchor').forEach(function (a) {
    a.addEventListener('click', function (e) {
      e.preventDefault();
      var href = a.getAttribute('href');
      if (!href) return;

      if (history.pushState) {
        history.pushState(null, '', href);
      }
      // smooth scroll to the section
      var target = document.querySelector(href);
      if (target) {
        var y = target.getBoundingClientRect().top + window.scrollY - 90;
        window.scrollTo({ top: y, behavior: 'smooth' });
      }

      // Copy URL to clipboard
      var url = window.location.origin + window.location.pathname + href;
      if (navigator.clipboard) {
        navigator.clipboard.writeText(url).catch(function () {});
      }
    });
  });

  /* ─── keyboard shortcut: cmd+k / ctrl+k to focus search? skip for now ─── */

  /* ─── initial page load: if URL has hash, scroll to it ─── */
  if (window.location.hash) {
    var hashTarget = document.querySelector(window.location.hash);
    if (hashTarget) {
      setTimeout(function () {
        var y = hashTarget.getBoundingClientRect().top + window.scrollY - 90;
        window.scrollTo({ top: y, behavior: 'smooth' });
      }, 200);
    }
  }

  /* ─── sidebar active on load ─── */
  var initialHash = window.location.hash.replace('#', '');
  if (initialHash) {
    navLinks.forEach(function (a) {
      a.classList.toggle('active', a.getAttribute('data-section') === initialHash);
    });
  } else {
    var first = document.querySelector('.sidebar-nav a[data-section]');
    if (first) first.classList.add('active');
  }

})();
